/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * The HTTPS server: serves the embedded pages (/, /map), the status JSON (/status, /cat), the CAT command
 * input (POST /cat) and the audio streams (/stream.wav, /stream.pcm). Every request goes through web_auth.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_system.h"
#include "build_info.h"
#include "cat.h"
#include "ota.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <math.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "usb_audio.h"
#include "web_auth.h"
#include "web_server.h"

#define STREAM_CHUNK_BYTES   4096
#define CAT_POST_MAX_BYTES   192     // largest accepted POST /cat body, including the NUL
#define CAT_JSON_BYTES       1152    // GET /cat response
#define STREAM_READ_TIMEOUT  pdMS_TO_TICKS(200)
#define KEEPALIVE_IDLE_READS 5       // idle waits (about 1 s) before a keep-alive silence block is sent
#define KEEPALIVE_FRAMES     64      // length of that block in audio frames
// Audio buffer between the USB reader and the network sender. A Wi-Fi / TCP stall then delays the audio
// instead of losing it: while the sender is blocked, the reader keeps draining the radio into this buffer.
// It lives in PSRAM when the board has it; without PSRAM a short internal buffer is used.
#define STREAM_BUF_MS        1500    // the browser plays at most about 1.2 s + 0.35 s behind anyway
#define STREAM_BUF_MS_SMALL  250
#define USB_READ_BYTES       2048    // one read from the USB audio driver
// Optional lower stream rate (?rate=24000 or ?rate=16000): a third of the data at 16 kHz, for a Wi-Fi
// link that cannot carry 48 kHz stereo (1.5 Mbit/s) steadily. Low-pass FIR, then keep every Nth frame.
#define DECIM_TAPS           64
#define DECIM_MAX_FACTOR     6
#define DECIM_MIN_RATE       8000
#define DECIM_MAX_CHANNELS   2
#define WAV_HEADER_SIZE      44

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char map_html_start[] asm("_binary_map_html_start");
extern const char map_html_end[] asm("_binary_map_html_end");
// Self-signed certificate and key (embedded as NUL-terminated PEM, the terminator is part of the length)
extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_pem_end[] asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");

typedef struct {
    httpd_req_t *req;
    bool wav;
    usb_audio_format_t fmt;     // what the radio sends
    uint32_t out_rate;          // what is streamed: fmt.sample_rate / decim
    uint8_t decim;              // 1 = as the radio sends it
} stream_ctx_t;

static const char *TAG = "web_server";
static atomic_bool s_stream_active;
static stream_ctx_t s_ctx;
static uint8_t s_chunk[STREAM_CHUNK_BYTES];

// Stream diagnostics: written only by stream_task, read (racy but harmless) by status_handler
#define SLOW_SEND_US 100000   // a chunk send that blocks this long means TCP/Wi-Fi back-pressure
static volatile uint32_t s_st_chunks;       // chunks sent
static volatile uint32_t s_st_usb_timeouts; // 200 ms without USB audio data
static volatile uint32_t s_st_buf_ms;       // capacity of the stream buffer
static volatile uint32_t s_st_buf_max_ms;   // most audio held in it (how long the longest stall was, roughly)
static volatile uint32_t s_st_overflows;    // times it filled up and the oldest audio was dropped

// Single reader (USB, core 0) / single sender (network, core 1) byte FIFO, protected by a mutex.
// The stream is a byte stream of whole frames; dropping is done in whole frames, so the alignment the
// client expects is kept.
typedef struct {
    uint8_t *data;
    size_t size;                 // multiple of frame_bytes
    size_t head;                 // next write
    size_t tail;                 // next read
    size_t fill;
    uint32_t frame_bytes;
    uint32_t bytes_per_ms;
    SemaphoreHandle_t lock;
    TaskHandle_t sender;
    volatile bool stop;          // set by the sender: the reader must finish
    volatile bool reader_done;   // set by the reader as its last action
    volatile bool device_gone;   // the USB audio device went away
} audio_fifo_t;

static audio_fifo_t s_fifo;
static uint8_t s_usb_read_buf[USB_READ_BYTES];
static volatile uint32_t s_st_rate;         // sample rate of the current / last stream

// Decimator state: only the USB reader task touches it while a stream runs
typedef struct {
    uint8_t factor;
    uint8_t channels;
    uint8_t phase;
    uint16_t pos;
    float taps[DECIM_TAPS];
    float hist[DECIM_MAX_CHANNELS][2 * DECIM_TAPS];   // each sample stored twice: a contiguous window
    uint8_t carry[2 * DECIM_MAX_CHANNELS];             // a frame split between two USB reads
    uint8_t carry_len;
} decimator_t;

static decimator_t s_dec;
static uint8_t s_dec_in[USB_READ_BYTES + sizeof(s_dec.carry)];
static int16_t s_dec_out[USB_READ_BYTES / 2];

static void decimator_init(decimator_t *d, uint8_t factor, uint8_t channels, uint32_t in_rate, uint32_t out_rate)
{
    memset(d, 0, sizeof(*d));
    d->factor = factor;
    d->channels = channels;
    // Windowed-sinc low-pass (Blackman), cut-off at 45 % of the output rate, unity gain at DC
    const float fc = 0.45f * (float)out_rate / (float)in_rate;
    const float mid = (DECIM_TAPS - 1) / 2.0f;
    float sum = 0;
    for (int k = 0; k < DECIM_TAPS; k++) {
        const float x = (float)k - mid;
        const float sinc = (x == 0.0f) ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * x) / ((float)M_PI * x);
        const float w = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * k / (DECIM_TAPS - 1)) +
                        0.08f * cosf(4.0f * (float)M_PI * k / (DECIM_TAPS - 1));
        d->taps[k] = sinc * w;
        sum += d->taps[k];
    }
    for (int k = 0; k < DECIM_TAPS; k++) {
        d->taps[k] /= sum;
    }
}

/** 16-bit little-endian frames in, decimated frames out (into s_dec_out). Returns the output byte count. */
static size_t decimate(decimator_t *d, const uint8_t *in, size_t n)
{
    const size_t frame_bytes = 2u * d->channels;
    memcpy(s_dec_in, d->carry, d->carry_len);
    memcpy(s_dec_in + d->carry_len, in, n);
    const size_t total = d->carry_len + n;
    const size_t frames = total / frame_bytes;
    d->carry_len = (uint8_t)(total - frames * frame_bytes);
    memcpy(d->carry, s_dec_in + frames * frame_bytes, d->carry_len);

    size_t out = 0;
    const uint8_t *p = s_dec_in;
    for (size_t f = 0; f < frames; f++, p += frame_bytes) {
        for (int c = 0; c < d->channels; c++) {
            const float v = (float)(int16_t)(p[2 * c] | (p[2 * c + 1] << 8));
            d->hist[c][d->pos] = v;
            d->hist[c][d->pos + DECIM_TAPS] = v;
        }
        d->pos = (uint16_t)((d->pos + 1) % DECIM_TAPS);
        if (++d->phase < d->factor) {
            continue;
        }
        d->phase = 0;
        for (int c = 0; c < d->channels; c++) {
            const float *h = &d->hist[c][d->pos];     // the last DECIM_TAPS samples, oldest first
            float acc = 0;
            for (int k = 0; k < DECIM_TAPS; k++) {
                acc += d->taps[k] * h[k];
            }
            const long r = lroundf(acc);
            s_dec_out[out++] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
        }
    }
    return out * sizeof(int16_t);
}

static void fifo_write(audio_fifo_t *f, const uint8_t *src, size_t n)
{
    if (n > f->size) {                          // cannot happen with USB_READ_BYTES, but stay safe
        src += n - f->size;
        n = f->size;
    }
    xSemaphoreTake(f->lock, portMAX_DELAY);
    const size_t space = f->size - f->fill;
    if (n > space) {
        // Full: drop the oldest audio, in whole frames, to keep the latency bounded
        size_t drop = ((n - space + f->frame_bytes - 1) / f->frame_bytes) * f->frame_bytes;
        if (drop > f->fill) {
            drop = f->fill;
        }
        f->tail = (f->tail + drop) % f->size;
        f->fill -= drop;
        s_st_overflows++;
    }
    const size_t first = (n < f->size - f->head) ? n : f->size - f->head;
    memcpy(f->data + f->head, src, first);
    memcpy(f->data, src + first, n - first);
    f->head = (f->head + n) % f->size;
    f->fill += n;
    const uint32_t ms = (uint32_t)(f->fill / f->bytes_per_ms);
    if (ms > s_st_buf_max_ms) {
        s_st_buf_max_ms = ms;
    }
    xSemaphoreGive(f->lock);
}

static size_t fifo_read(audio_fifo_t *f, uint8_t *dst, size_t max)
{
    xSemaphoreTake(f->lock, portMAX_DELAY);
    const size_t n = (f->fill < max) ? f->fill : max;
    const size_t first = (n < f->size - f->tail) ? n : f->size - f->tail;
    memcpy(dst, f->data + f->tail, first);
    memcpy(dst + first, f->data, n - first);
    f->tail = (f->tail + n) % f->size;
    f->fill -= n;
    xSemaphoreGive(f->lock);
    return n;
}

/** Drains the radio's USB audio into the FIFO, whatever the network is doing */
static void usb_reader_task(void *arg)
{
    audio_fifo_t *f = (audio_fifo_t *)arg;
    while (!f->stop) {
        uint32_t n = 0;
        const esp_err_t r = usb_audio_read(s_usb_read_buf, sizeof(s_usb_read_buf), &n, STREAM_READ_TIMEOUT);
        if (r == ESP_ERR_TIMEOUT) {
            s_st_usb_timeouts++;
            continue;
        }
        if (r != ESP_OK) {
            f->device_gone = true;
            break;
        }
        if (n > sizeof(s_usb_read_buf)) {
            n = sizeof(s_usb_read_buf);
        }
        if (n > 0 && s_dec.factor > 1) {
            n = (uint32_t)decimate(&s_dec, s_usb_read_buf, n);
            if (n > 0) {
                fifo_write(f, (const uint8_t *)s_dec_out, n);
            }
            xTaskNotifyGive(f->sender);
        } else if (n > 0) {
            fifo_write(f, s_usb_read_buf, n);
            xTaskNotifyGive(f->sender);
        }
    }
    f->reader_done = true;
    xTaskNotifyGive(f->sender);
    vTaskDelete(NULL);
}
static volatile uint32_t s_st_slow_sends;   // sends slower than SLOW_SEND_US
static volatile uint32_t s_st_max_send_ms;  // slowest send
static volatile uint32_t s_st_starts;       // streams started since boot

// Connection diagnostics: every new TLS session costs tens of KB of internal RAM while it is set up,
// so /status reports how many sessions were opened and closed since boot
static volatile uint32_t s_st_conn_opened;
static volatile uint32_t s_st_conn_closed;

static void session_closed(void *ctx)
{
    (void)ctx;
    s_st_conn_closed++;
}

/** Call at the start of a handler: counts the session the first time it sends a request */
static void note_session(httpd_req_t *req)
{
    static const char marker = 1;
    const int fd = httpd_req_to_sockfd(req);
    if (httpd_sess_get_ctx(req->handle, fd) == NULL) {
        httpd_sess_set_ctx(req->handle, fd, (void *)&marker, session_closed);
        s_st_conn_opened++;
    }
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

/** Build a WAV header with an open-ended (0xFFFFFFFF) data length for live streaming */
static void build_wav_header(uint8_t *h, const usb_audio_format_t *fmt)
{
    const uint16_t block_align = fmt->channels * fmt->subframe_size;
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 0xFFFFFFFF);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);                       // PCM
    put_le16(h + 22, fmt->channels);
    put_le32(h + 24, fmt->sample_rate);
    put_le32(h + 28, fmt->sample_rate * block_align);
    put_le16(h + 32, block_align);
    put_le16(h + 34, fmt->subframe_size * 8);  // USB audio samples are left-justified in the subframe
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, 0xFFFFFFFF);
}

static void stream_task(void *arg)
{
    stream_ctx_t *ctx = (stream_ctx_t *)arg;
    httpd_req_t *req = ctx->req;
    char rate_s[12], ch_s[4], width_s[4];
    const uint32_t frame_bytes = ctx->fmt.channels * ctx->fmt.subframe_size;
    const uint32_t chunk_bytes = (STREAM_CHUNK_BYTES / frame_bytes) * frame_bytes;

    httpd_resp_set_type(req, ctx->wav ? "audio/wav" : "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    web_auth_set_common_headers(req);
    if (!ctx->wav) {
        snprintf(rate_s, sizeof(rate_s), "%lu", (unsigned long)ctx->out_rate);
        snprintf(ch_s, sizeof(ch_s), "%u", ctx->fmt.channels);
        snprintf(width_s, sizeof(width_s), "%u", ctx->fmt.subframe_size);
        httpd_resp_set_hdr(req, "X-Sample-Rate", rate_s);
        httpd_resp_set_hdr(req, "X-Channels", ch_s);
        httpd_resp_set_hdr(req, "X-Bytes-Per-Sample", width_s);
    }

    esp_err_t err = ESP_OK;
    if (ctx->wav) {
        uint8_t header[WAV_HEADER_SIZE];
        usb_audio_format_t out_fmt = ctx->fmt;
        out_fmt.sample_rate = ctx->out_rate;
        build_wav_header(header, &out_fmt);
        err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    }

    usb_audio_flush();   // start from live audio, not stale buffered data
    s_st_chunks = s_st_usb_timeouts = s_st_slow_sends = s_st_max_send_ms = 0;
    s_st_buf_max_ms = s_st_overflows = 0;
    s_st_starts++;

    // The FIFO between the USB reader and this task
    audio_fifo_t *f = &s_fifo;
    const uint32_t bytes_per_ms = (ctx->out_rate * frame_bytes + 999) / 1000;
    s_st_rate = ctx->out_rate;
    s_dec.factor = 1;
    if (ctx->decim > 1) {
        decimator_init(&s_dec, ctx->decim, ctx->fmt.channels, ctx->fmt.sample_rate, ctx->out_rate);
    }
    size_t buf_ms = STREAM_BUF_MS;
    size_t size = ((bytes_per_ms * buf_ms) / frame_bytes) * frame_bytes;
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        buf_ms = STREAM_BUF_MS_SMALL;
        size = ((bytes_per_ms * buf_ms) / frame_bytes) * frame_bytes;
        data = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (f->lock == NULL) {
        f->lock = xSemaphoreCreateMutex();
    }
    if (data == NULL || f->lock == NULL) {
        ESP_LOGE(TAG, "No memory for the stream buffer");
        err = ESP_ERR_NO_MEM;
    } else {
        f->data = data;
        f->size = size;
        f->head = f->tail = f->fill = 0;
        f->frame_bytes = frame_bytes;
        f->bytes_per_ms = bytes_per_ms;
        f->sender = xTaskGetCurrentTaskHandle();
        f->stop = false;
        f->reader_done = false;
        f->device_gone = false;
        s_st_buf_ms = (uint32_t)buf_ms;
        ulTaskNotifyTake(pdTRUE, 0);
        // Core 0 with the USB host; above the httpd task so a slow network never starves it
        if (xTaskCreatePinnedToCore(usb_reader_task, "usb_rd", 3072, f, 6, NULL, 0) != pdPASS) {
            ESP_LOGE(TAG, "Could not start the USB reader");
            f->reader_done = true;
            err = ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "Stream started (%s, %lu Hz), buffer %u ms in %s", ctx->wav ? "wav" : "pcm",
             (unsigned long)ctx->out_rate, (unsigned)buf_ms,
             (data != NULL && esp_ptr_external_ram(data)) ? "PSRAM" : "internal RAM");

    uint32_t idle_waits = 0;
    while (err == ESP_OK) {
        const size_t n = fifo_read(f, s_chunk, chunk_bytes);
        if (n == 0) {
            if (f->reader_done) {
                break;   // device gone (the FIFO is empty)
            }
            if (ulTaskNotifyTake(pdTRUE, STREAM_READ_TIMEOUT) == 0) {
                // Without audio nothing is sent, so a vanished client would never be noticed and
                // s_stream_active would stay set, rejecting every later stream until reboot. Send a short
                // block of silence about once a second: the failed send ends the task.
                if (++idle_waits >= KEEPALIVE_IDLE_READS) {
                    idle_waits = 0;
                    const uint32_t silence = (KEEPALIVE_FRAMES * frame_bytes < chunk_bytes) ? KEEPALIVE_FRAMES * frame_bytes
                                                                                             : chunk_bytes;
                    memset(s_chunk, 0, silence);
                    err = httpd_resp_send_chunk(req, (const char *)s_chunk, silence);
                }
            }
            continue;
        }
        idle_waits = 0;
        const int64_t t0 = esp_timer_get_time();
        err = httpd_resp_send_chunk(req, (const char *)s_chunk, n);
        const int64_t dt = esp_timer_get_time() - t0;
        s_st_chunks++;
        if (dt > SLOW_SEND_US) {
            s_st_slow_sends++;
        }
        if ((uint32_t)(dt / 1000) > s_st_max_send_ms) {
            s_st_max_send_ms = (uint32_t)(dt / 1000);
        }
    }

    // Stop the reader and wait for it before the buffer is freed (it notices within one read timeout)
    f->stop = true;
    for (int i = 0; i < 20 && !f->reader_done; i++) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    if (f->reader_done) {
        free(f->data);
    } else {
        ESP_LOGE(TAG, "USB reader did not stop: keeping its buffer");   // never freed under a running task
    }
    f->data = NULL;
    ESP_LOGI(TAG, "Stream ended");

    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    atomic_store(&s_stream_active, false);
    vTaskDelete(NULL);
}

static esp_err_t start_stream(httpd_req_t *req, bool wav)
{
    note_session(req);
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }

    usb_audio_format_t fmt;
    // The format comes from a USB device descriptor: refuse values the stream code cannot handle
    // (a zero frame size would divide by zero, a huge one would not fit the chunk buffer)
    if (!usb_audio_get_format(&fmt) || fmt.channels == 0 || fmt.channels > 8 ||
        fmt.subframe_size == 0 || fmt.subframe_size > 4 || fmt.sample_rate == 0) {
        web_auth_set_common_headers(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "No usable USB audio device streaming", HTTPD_RESP_USE_STRLEN);
    }

    // Optional ?rate=N: a lower stream rate, an exact divisor of the radio's rate (16-bit audio only)
    uint32_t out_rate = fmt.sample_rate;
    uint8_t decim = 1;
    char query[48], value[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "rate", value, sizeof(value)) == ESP_OK) {
        const unsigned long want = strtoul(value, NULL, 10);
        if (want >= DECIM_MIN_RATE && want < fmt.sample_rate && fmt.sample_rate % want == 0 &&
            fmt.sample_rate / want <= DECIM_MAX_FACTOR && fmt.subframe_size == 2 &&
            fmt.channels <= DECIM_MAX_CHANNELS) {
            out_rate = (uint32_t)want;
            decim = (uint8_t)(fmt.sample_rate / want);
        }
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_stream_active, &expected, true)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Another client is already streaming", HTTPD_RESP_USE_STRLEN);
    }

    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        atomic_store(&s_stream_active, false);
        return httpd_resp_send_500(req);
    }

    s_ctx = (stream_ctx_t) {.req = async_req, .wav = wav, .fmt = fmt, .out_rate = out_rate, .decim = decim};
    // TLS record encryption runs in this task, so it needs more stack than plain HTTP
    // Core 1: keep TLS encryption and sending away from Wi-Fi / USB, which run on core 0
    if (xTaskCreatePinnedToCore(stream_task, "stream", 8192, &s_ctx, 5, NULL, 1) != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        atomic_store(&s_stream_active, false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    web_auth_set_common_headers(req);
    web_auth_set_html_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t map_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    web_auth_set_common_headers(req);
    web_auth_set_html_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, map_html_start, map_html_end - map_html_start - 1);
}

/** Send a JSON document that was formatted with snprintf; a truncated one is an error, never sent */
static esp_err_t send_json(httpd_req_t *req, const char *json, int formatted, size_t capacity)
{
    web_auth_set_common_headers(req);
    if (formatted < 0 || (size_t)formatted >= capacity) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal error");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, formatted);
}

/** Copy text into a JSON string body: printable ASCII only, '"' and '\' escaped, always NUL-terminated */
static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t n = 0;
    for (; *in != '\0'; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c > 0x7e) {
            c = '.';
        }
        if (c == '"' || c == '\\') {
            if (n + 2 >= out_size) {
                break;
            }
            out[n++] = '\\';
        } else if (n + 1 >= out_size) {
            break;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

/** A mode code as reported by the radio, restricted to what can safely sit inside a JSON string */
static char safe_mode_char(char c)
{
    return ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) ? c : ' ';
}

static esp_err_t status_handler(httpd_req_t *req)
{
    note_session(req);
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    usb_audio_format_t fmt;
    char json[768];
    int n;
    if (usb_audio_get_format(&fmt)) {
        n = snprintf(json, sizeof(json),
                     "{\"streaming\":true,\"sample_rate\":%lu,\"channels\":%u,\"bits\":%u,\"bytes_per_sample\":%u,\"version\":\"%s\",\"build\":\"%s\"",
                     (unsigned long)fmt.sample_rate, fmt.channels, fmt.bit_resolution, fmt.subframe_size,
                     FTX1_VERSION, build_info_get());
    } else {
        n = snprintf(json, sizeof(json), "{\"streaming\":false,\"version\":\"%s\",\"build\":\"%s\"", FTX1_VERSION, build_info_get());
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        wifi_ap_record_t ap;
        const int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        // heap_* include PSRAM when it is enabled; int_* are the internal RAM only
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"uptime_s\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,\"heap_block\":%lu,"
                      "\"int_free\":%lu,\"int_min\":%lu,\"int_block\":%lu,\"rssi\":%d,"
                      "\"conn_opened\":%lu,\"conn_closed\":%lu,"
                      "\"stream\":{\"starts\":%lu,\"chunks\":%lu,\"usb_timeouts\":%lu,\"slow_sends\":%lu,\"max_send_ms\":%lu,"
                      "\"buf_ms\":%lu,\"buf_max_ms\":%lu,\"overflows\":%lu,\"rate\":%lu}}",
                      (unsigned long)(esp_timer_get_time() / 1000000),
                      (unsigned long)esp_get_free_heap_size(),
                      (unsigned long)esp_get_minimum_free_heap_size(),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      rssi,
                      (unsigned long)s_st_conn_opened, (unsigned long)s_st_conn_closed,
                      (unsigned long)s_st_starts, (unsigned long)s_st_chunks,
                      (unsigned long)s_st_usb_timeouts, (unsigned long)s_st_slow_sends,
                      (unsigned long)s_st_max_send_ms,
                      (unsigned long)s_st_buf_ms, (unsigned long)s_st_buf_max_ms, (unsigned long)s_st_overflows,
                      (unsigned long)s_st_rate);
    }
    return send_json(req, json, n, sizeof(json));
}

static esp_err_t cat_handler(httpd_req_t *req)
{
    note_session(req);
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    cat_state_t st;
    char note[2 * sizeof(st.last_error) + 1];
    char json[CAT_JSON_BYTES];
    cat_get_state(&st);
    json_escape(st.last_error, note, sizeof(note));   // contains bytes echoed by the radio
    char rig_name[2 * sizeof(st.rig_name) + 1];
    char rig_id[2 * sizeof(st.rig_id) + 1];
    json_escape(st.rig_name, rig_name, sizeof(rig_name));
    json_escape(st.rig_id, rig_id, sizeof(rig_id));      // only alphanumerics or '?', escaped anyway
    int n = snprintf(json, sizeof(json),
             "{\"connected\":%s,\"rig\":\"%s\",\"rig_id\":\"%s\",\"dual_rx\":%s,\"can_control\":%s,"
             "\"c4fm\":%s,\"sq_max\":%u,\"freq_min\":%lu,\"freq_max\":%lu,"
             "\"busy\":%s,\"tx\":%s,\"s_main\":%u,\"s_sub\":%u,"
             "\"freq_main\":%lu,\"freq_sub\":%lu,"
             "\"mode_main\":\"%s\",\"mode_sub\":\"%s\","
             "\"mode_main_raw\":\"%c\",\"mode_sub_raw\":\"%c\","
             "\"vol_main\":%u,\"vol_sub\":%u,"
             "\"sq_main\":%u,\"sq_sub\":%u,"
             "\"notch_main\":%s,\"notch_sub\":%s,\"notch_hz_main\":%u,\"notch_hz_sub\":%u,"
             "\"age_ms\":%lu,\"devices\":%u,\"cp210x\":%u,\"answered\":%u,\"note\":\"%s\"}",
             st.connected ? "true" : "false", rig_name, rig_id,
             st.dual_rx ? "true" : "false", st.can_control ? "true" : "false", st.c4fm ? "true" : "false",
             st.sq_max, (unsigned long)st.freq_min, (unsigned long)st.freq_max,
             st.busy ? "true" : "false",
             st.tx ? "true" : "false",
             st.s_main, st.s_sub,
             (unsigned long)st.freq_main, (unsigned long)st.freq_sub,
             st.mode_main, st.mode_sub,
             safe_mode_char(st.mode_main_raw),
             safe_mode_char(st.mode_sub_raw),
             st.vol_main, st.vol_sub,
             st.sq_main, st.sq_sub,
             st.notch_main ? "true" : "false", st.notch_sub ? "true" : "false",
             st.notch_hz_main, st.notch_hz_sub,
             (unsigned long)st.age_ms, st.devices, st.cp210x, st.answered, note);
    return send_json(req, json, n, sizeof(json));
}

// ---- POST /cat: strict, allocation free JSON reader ------------------------------------------------
// Accepted body: one flat JSON object, e.g. {"target":"sub","freq":14074000,"mode":"USB"}
//   target  "main" | "sub"                 (default "main")
//   freq    integer Hz                     (range checked again in cat.c)
//   mode    string, one of the CAT mode names or a single mode code
//   vol, sq integer 0-255
//   notch   0 | 1 | false | true           notch_hz integer 10-3200
//   power   0 | 1 | false | true
//   action  "swap" | "copy_main_to_sub" | "copy_sub_to_main"
// Unknown or repeated keys, escapes, nesting and trailing data are rejected. There is no way to send a
// raw CAT string.
typedef struct {
    bool sub;
    bool has_target;
    bool has_freq;
    uint32_t freq;
    bool has_mode;
    char mode[16];
    bool has_vol;
    uint8_t vol;
    bool has_sq;
    uint8_t sq;
    bool has_notch;
    bool notch;
    bool has_notch_hz;
    uint16_t notch_hz;
    bool has_power;
    bool power;
    bool has_action;
    char action[24];
} cat_req_t;

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') {
        (*p)++;
    }
}

/** A quoted string: printable ASCII, no escapes, shorter than out_size. Stops at the NUL of the input. */
static bool parse_string(const char **p, char *out, size_t out_size)
{
    if (**p != '"') {
        return false;
    }
    (*p)++;
    size_t n = 0;
    while (**p != '"') {
        const unsigned char c = (unsigned char)**p;
        if (c < 0x20 || c > 0x7e || c == '\\' || n + 1 >= out_size) {
            return false;
        }
        out[n++] = (char)c;
        (*p)++;
    }
    out[n] = '\0';
    (*p)++;
    return true;
}

/** A bare value: digits, '-' and lower-case letters (numbers, true, false, null) */
static bool parse_token(const char **p, char *out, size_t out_size)
{
    size_t n = 0;
    while ((**p >= '0' && **p <= '9') || (**p >= 'a' && **p <= 'z') || **p == '-') {
        if (n + 1 >= out_size) {
            return false;
        }
        out[n++] = **p;
        (*p)++;
    }
    out[n] = '\0';
    return n > 0;
}

static bool token_to_uint(const char *t, uint32_t max, uint32_t *out)
{
    const size_t len = strlen(t);
    if (len == 0 || len > 10) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (t[i] < '0' || t[i] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(t[i] - '0');
    }
    if (v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool token_to_bool(const char *t, bool *out)
{
    if (strcmp(t, "1") == 0 || strcmp(t, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(t, "0") == 0 || strcmp(t, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_cat_request(const char *s, cat_req_t *r)
{
    memset(r, 0, sizeof(*r));
    skip_ws(&s);
    if (*s++ != '{') {
        return false;
    }
    for (;;) {
        skip_ws(&s);
        char key[12];
        if (!parse_string(&s, key, sizeof(key))) {
            return false;
        }
        skip_ws(&s);
        if (*s++ != ':') {
            return false;
        }
        skip_ws(&s);

        char text[24] = {0};      // string value
        char token[12] = {0};     // bare value
        const bool is_text = (*s == '"');
        if (is_text ? !parse_string(&s, text, sizeof(text)) : !parse_token(&s, token, sizeof(token))) {
            return false;
        }

        uint32_t num = 0;
        if (strcmp(key, "target") == 0) {
            if (!is_text || r->has_target) return false;
            if (strcmp(text, "main") == 0) r->sub = false;
            else if (strcmp(text, "sub") == 0) r->sub = true;
            else return false;
            r->has_target = true;
        } else if (strcmp(key, "freq") == 0) {
            if (is_text || r->has_freq || !token_to_uint(token, UINT32_MAX, &r->freq)) return false;
            r->has_freq = true;
        } else if (strcmp(key, "mode") == 0) {
            if (!is_text || r->has_mode || text[0] == '\0') return false;
            strlcpy(r->mode, text, sizeof(r->mode));
            r->has_mode = true;
        } else if (strcmp(key, "vol") == 0) {
            if (is_text || r->has_vol || !token_to_uint(token, 255, &num)) return false;
            r->vol = (uint8_t)num;
            r->has_vol = true;
        } else if (strcmp(key, "sq") == 0) {
            if (is_text || r->has_sq || !token_to_uint(token, 255, &num)) return false;
            r->sq = (uint8_t)num;
            r->has_sq = true;
        } else if (strcmp(key, "notch") == 0) {
            if (is_text || r->has_notch || !token_to_bool(token, &r->notch)) return false;
            r->has_notch = true;
        } else if (strcmp(key, "notch_hz") == 0) {
            if (is_text || r->has_notch_hz || !token_to_uint(token, 3200, &num) || num < 10) return false;
            r->notch_hz = (uint16_t)num;
            r->has_notch_hz = true;
        } else if (strcmp(key, "power") == 0) {
            if (is_text || r->has_power || !token_to_bool(token, &r->power)) return false;
            r->has_power = true;
        } else if (strcmp(key, "action") == 0) {
            if (!is_text || r->has_action) return false;
            strlcpy(r->action, text, sizeof(r->action));
            r->has_action = true;
        } else {
            return false;   // unknown key
        }

        skip_ws(&s);
        if (*s == ',') {
            s++;
            continue;
        }
        if (*s == '}') {
            s++;
            break;
        }
        return false;
    }
    skip_ws(&s);
    return *s == '\0';
}

/** Read the whole body (1 .. buf_size-1 bytes) into buf as a NUL-terminated string */
static bool read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    const size_t total = req->content_len;
    if (total == 0 || total >= buf_size) {
        return false;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, buf + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return false;
            }
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return strlen(buf) == total;   // an embedded NUL would hide data from the parser
}

static bool content_type_is_json(httpd_req_t *req)
{
    char type[48];
    const size_t len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (len == 0 || len >= sizeof(type) || httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
        return false;
    }
    static const char json_type[] = "application/json";
    return strncasecmp(type, json_type, sizeof(json_type) - 1) == 0;
}

static esp_err_t cat_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, true)) {
        return ESP_OK;
    }
    web_auth_set_common_headers(req);
    // The body may stay unread on an early error, so do not reuse the connection
    httpd_resp_set_hdr(req, "Connection", "close");

    if (!content_type_is_json(req)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Type must be application/json");
    }
    if (req->content_len >= CAT_POST_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Request body is too large");
    }

    char buf[CAT_POST_MAX_BYTES];
    cat_req_t r;
    if (!read_body(req, buf, sizeof(buf)) || !parse_cat_request(buf, &r)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    }

    esp_err_t err = ESP_OK;
    int ops = 0;
#define APPLY(call) do { esp_err_t e_ = (call); if (e_ != ESP_OK) { err = e_; } ops++; } while (0)
    if (r.has_action) {
        if (strcmp(r.action, "swap") == 0) APPLY(cat_swap_vfo());
        else if (strcmp(r.action, "copy_main_to_sub") == 0) APPLY(cat_copy_main_to_sub());
        else if (strcmp(r.action, "copy_sub_to_main") == 0) APPLY(cat_copy_sub_to_main());
        else err = ESP_ERR_INVALID_ARG, ops++;
    }
    if (r.has_freq) APPLY(cat_set_freq(r.sub, r.freq));
    if (r.has_mode) APPLY(cat_set_mode(r.sub, r.mode));
    if (r.has_vol) APPLY(cat_set_volume(r.sub, r.vol));
    if (r.has_sq) APPLY(cat_set_squelch(r.sub, r.sq));
    if (r.has_notch) APPLY(cat_set_notch(r.sub, r.notch));
    if (r.has_notch_hz) APPLY(cat_set_notch_freq(r.sub, r.notch_hz));
    if (r.has_power) APPLY(cat_set_power(r.power));
#undef APPLY

    httpd_resp_set_type(req, "application/json");
    if (ops > 0 && err == ESP_OK) {
        return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Request rejected\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, true)) {
        return ESP_OK;
    }
    web_auth_set_common_headers(req);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "Reboot requested via web API");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t pcm_handler(httpd_req_t *req)
{
    return start_stream(req, false);
}

static esp_err_t wav_handler(httpd_req_t *req)
{
    return start_stream(req, true);
}

esp_err_t web_server_start(void)
{
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.servercert = servercert_pem_start;
    config.servercert_len = servercert_pem_end - servercert_pem_start;
    config.prvtkey_pem = prvtkey_pem_start;
    config.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;
    // tls_version is left at ESP_TLS_VER_ANY: this mbedTLS build only provides TLS 1.2 and 1.3

    // Chrome (mobile/desktop) opens several parallel connections and TLS handshakes occupy
    // sockets until complete. The default of 4 fills up immediately, causing "Too many retries".
    // 8 is enough for a single browser client (page + /status poll + /cat poll + stream).
    config.httpd.max_open_sockets = 8;
    // Run the httpd task (including TLS handshakes) on core 1, away from Wi-Fi / USB on core 0
    config.httpd.core_id = 1;
    // The TLS handshake runs synchronously inside the single httpd task. Chrome (especially on Android)
    // opens speculative connections that never send a ClientHello; with the 10 s default each of them
    // freezes the whole server and the real request ends in ERR_TOO_MANY_RETRIES. A normal handshake
    // takes about 0.6 s here, so 3 s leaves plenty of margin even on a weak Wi-Fi link.
    config.tls_handshake_timeout_ms = 3000;
    // When all sockets are taken, drop the least recently used one instead of refusing new clients,
    // so idle or half-open connections cannot starve a legitimate user
    config.httpd.lru_purge_enable = true;
    // The default of 8 URI handlers is exactly used up by the routes below plus /ota
    config.httpd.max_uri_handlers = 16;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_ssl_start(&server, &config), TAG, "httpd_ssl_start");

    static const httpd_uri_t uris[] = {
        {.uri = "/",           .method = HTTP_GET,  .handler = index_handler},
        {.uri = "/map",        .method = HTTP_GET,  .handler = map_handler},
        {.uri = "/status",     .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/cat",        .method = HTTP_GET,  .handler = cat_handler},
        {.uri = "/cat",        .method = HTTP_POST, .handler = cat_post_handler},
        {.uri = "/reboot",     .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/stream.pcm", .method = HTTP_GET,  .handler = pcm_handler},
        {.uri = "/stream.wav", .method = HTTP_GET,  .handler = wav_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register handler");
    }
    ESP_RETURN_ON_ERROR(ota_register(server), TAG, "register ota");
    ESP_LOGI(TAG, "HTTPS server started on port %d", config.port_secure);
    web_auth_log_startup();
    return ESP_OK;
}
