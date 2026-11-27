/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * The HTTPS server: serves the embedded pages (/, /map), the status JSON (/status, /cat), the CAT command
 * input (POST /cat) and the audio streams (/stream.wav, /stream.pcm). Every request goes through web_auth.
 */

#include <errno.h>
#include <fcntl.h>
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
#include "https_identity.h"
#include "ft8_tx.h"
#include "esp_system.h"
#include "esp_random.h"
#include "build_info.h"
#include "acc_scope.h"
#include "cat.h"
#include "ota.h"
#include "app_config.h"
#include "tx.h"
#include "tx_voice.h"
#include "rigctld.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <math.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "usb_audio.h"
#include "web_auth.h"
#include "ui_prefs.h"
#include "nvs.h"
#include "web_server.h"

#define WEB_CORE             0
#define STREAM_CHUNK_BYTES   4096
#define CAT_POST_MAX_BYTES   192     // largest accepted POST /cat body, including the NUL
#define CAT_JSON_BYTES       1408    // GET /cat response
#define STREAM_READ_TIMEOUT  pdMS_TO_TICKS(200)
#define KEEPALIVE_IDLE_READS 5       // idle waits (about 1 s) before a keep-alive silence block is sent
#define KEEPALIVE_FRAMES     64      // length of that block in audio frames
// Audio buffer between the USB reader and the network sender. A Wi-Fi / TCP stall then delays the audio
// instead of losing it: while the sender is blocked, the reader keeps draining the radio into this buffer.
// It lives in PSRAM when the board has it; without PSRAM a short internal buffer is used.
#define STREAM_BUF_MS        1500    // the browser plays at most about 1.2 s + 0.35 s behind anyway
#define STREAM_BUF_MS_SMALL  250
#define USB_READ_BYTES       2048    // one read from the USB audio driver
#define OVERFLOW_DROP_MS     300     // audio dropped at once when the stream buffer is full
// Optional lower stream rate (?rate=24000 or ?rate=16000): a third of the data at 16 kHz, for a Wi-Fi
// link that cannot carry 48 kHz stereo (1.5 Mbit/s) steadily. Low-pass FIR, then keep every Nth frame.
#define DECIM_TAPS           64
#define DECIM_MAX_FACTOR     6
#define DECIM_MIN_RATE       8000
#define DECIM_MAX_CHANNELS   2
#define WAV_HEADER_SIZE      44

extern const char tx_mic_js_start[] asm("_binary_tx_mic_js_start");
extern const char tx_mic_js_end[] asm("_binary_tx_mic_js_end");
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
static atomic_uint s_scope_clients;   // open /scope.bin streams (ACC scope)
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
    uint32_t sample_rate;
    SemaphoreHandle_t lock;
    TaskHandle_t sender;
    volatile bool stop;          // set by the sender: the reader must finish
    volatile bool reader_done;   // set by the reader as its last action
    volatile bool device_gone;   // the USB audio device went away
} audio_fifo_t;

static audio_fifo_t s_fifo;
static uint8_t s_usb_read_buf[USB_READ_BYTES];
static volatile uint32_t s_st_rate;         // sample rate of the current / last stream
// Measure between the first and latest data reads. Time spent after playback ends
// must not lower the reported rate for the completed stream.
static portMUX_TYPE s_usb_rate_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint64_t bytes;
    int64_t first_us;
    int64_t last_us;
    uint32_t frame_bytes;
} s_usb_rate;

static uint64_t usb_input_rate_dhz(void)
{
    portENTER_CRITICAL(&s_usb_rate_lock);
    const int64_t elapsed_us = s_usb_rate.last_us - s_usb_rate.first_us;
    const uint64_t bytes = s_usb_rate.bytes;
    const uint32_t frame_bytes = s_usb_rate.frame_bytes;
    portEXIT_CRITICAL(&s_usb_rate_lock);
    if (elapsed_us <= 1000000 || frame_bytes == 0) {
        return 0;
    }
    return (bytes / frame_bytes) * 10000000ULL / (uint64_t)elapsed_us;
}

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
        // Full: drop the oldest audio, in whole frames, to keep the latency bounded. Drop a good piece at
        // once (OVERFLOW_DROP_MS): one short gap is heard as a gap, many tiny ones as crackles.
        size_t drop = ((n - space + f->frame_bytes - 1) / f->frame_bytes) * f->frame_bytes;
        const size_t chunk = ((f->bytes_per_ms * OVERFLOW_DROP_MS) / f->frame_bytes) * f->frame_bytes;
        if (drop < chunk) {
            drop = chunk;
        }
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
    bool paused = false;
    while (!f->stop) {
        uint32_t n = 0;
        const esp_err_t r = usb_audio_read(s_usb_read_buf, sizeof(s_usb_read_buf), &n, STREAM_READ_TIMEOUT);
        if (r == ESP_ERR_NOT_FINISHED) {
            if (!paused) {
                xSemaphoreTake(f->lock, portMAX_DELAY);
                f->tail = f->head;
                f->fill = 0;
                xSemaphoreGive(f->lock);
                paused = true;
            }
            // Keep browser time moving at the output rate, without counting
            // synthetic silence as USB input or running it through the FIR.
            const size_t silence_bytes = (f->sample_rate / 100) * f->frame_bytes;
            const size_t bytes = silence_bytes < sizeof(s_usb_read_buf) ? silence_bytes :
                                 sizeof(s_usb_read_buf) / f->frame_bytes * f->frame_bytes;
            memset(s_usb_read_buf, 0, bytes);
            fifo_write(f, s_usb_read_buf, bytes);
            xTaskNotifyGive(f->sender);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (r == ESP_ERR_TIMEOUT) {
            if (!paused) {
                s_st_usb_timeouts++;
            }
            continue;
        }
        if (r != ESP_OK) {
            f->device_gone = true;
            break;
        }
        if (n > sizeof(s_usb_read_buf)) {
            n = sizeof(s_usb_read_buf);
        }
        if (n > 0) {
            const int64_t now_us = esp_timer_get_time();
            portENTER_CRITICAL(&s_usb_rate_lock);
            if (paused || s_usb_rate.first_us == 0) {
                s_usb_rate.bytes = 0;
                s_usb_rate.first_us = now_us;
                paused = false;
            } else {
                s_usb_rate.bytes += n;     // the first read only marks the start
            }
            s_usb_rate.last_us = now_us;
            portEXIT_CRITICAL(&s_usb_rate_lock);
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

static uint32_t s_boot_id;

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
    portENTER_CRITICAL(&s_usb_rate_lock);
    s_usb_rate.bytes = 0;
    s_usb_rate.first_us = 0;
    s_usb_rate.last_us = 0;
    s_usb_rate.frame_bytes = frame_bytes;
    portEXIT_CRITICAL(&s_usb_rate_lock);
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
        f->sample_rate = ctx->out_rate;
        f->sender = xTaskGetCurrentTaskHandle();
        f->stop = false;
        f->reader_done = false;
        f->device_gone = false;
        s_st_buf_ms = (uint32_t)buf_ms;
        ulTaskNotifyTake(pdTRUE, 0);
        // Core 1, away from the Wi-Fi driver and LwIP on core 0, at 19: above LwIP (18), the web server and
        // the stream sender, below the USB host tasks (20). At 6 on core 0 it fell behind while the network
        // was busy, the UAC driver's buffer filled, and about 1.3 % of the radio's audio was dropped there.
        if (xTaskCreatePinnedToCore(usb_reader_task, "usb_rd", 3072, f, 19, NULL, 1) != pdPASS) {
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
    const uint64_t usb_in_dhz = usb_input_rate_dhz();
    ESP_LOGI(TAG, "Stream ended (%s), fd=%d, chunks=%lu, usb_in_hz=%lu.%lu, usb_bus_resets=%lu",
             f->device_gone ? "USB input read failed" : esp_err_to_name(err), httpd_req_to_sockfd(req),
             (unsigned long)s_st_chunks, (unsigned long)(usb_in_dhz / 10), (unsigned long)(usb_in_dhz % 10),
             (unsigned long)usb_audio_bus_resets());

    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    atomic_store(&s_stream_active, false);
    vTaskDelete(NULL);
}

static esp_err_t start_stream(httpd_req_t *req, bool wav)
{
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
    if (xTaskCreatePinnedToCore(stream_task, "stream", 8192, &s_ctx, 5, NULL, WEB_CORE) != pdPASS) {
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

static esp_err_t tx_mic_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) return ESP_OK;
    web_auth_set_common_headers(req);
    httpd_resp_set_type(req, "text/javascript; charset=utf-8");
    return httpd_resp_send(req, tx_mic_js_start, tx_mic_js_end - tx_mic_js_start - 1);
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

static atomic_uint s_tls_opened, s_tls_closed, s_tls_handshake_errors;

static void count_tls_events(esp_https_server_user_cb_arg_t *event)
{
    switch (event->user_cb_state) {
    case HTTPD_SSL_USER_CB_SESS_CREATE:
        atomic_fetch_add(&s_tls_opened, 1);
        break;
    case HTTPD_SSL_USER_CB_SESS_CLOSE:
        atomic_fetch_add(&s_tls_closed, 1);
        break;
    case HTTPD_SSL_USER_CB_SESS_ERROR:
        atomic_fetch_add(&s_tls_handshake_errors, 1);
        break;
    default:
        break;
    }
}

static const char *tx_reason(const tx_state_t *tx, char formatted[32])
{
    if (!tx->stop_swr) {
        return tx->reason;
    }
    const unsigned tenths = (tx->stop_swr + 5) / 10;
    snprintf(formatted, 32, "SWR %u.%u:1 (%s)", tenths / 10, tenths % 10, tx->reason);
    return formatted;
}

static unsigned lwip_sockets_used(void)
{
    // ESP-IDF reserves this contiguous descriptor range for lwIP. F_GETFL is a
    // local descriptor check and doesn't make a TCP round trip or allocate a socket.
    const int saved_errno = errno;
    unsigned used = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
        if (lwip_fcntl(fd, F_GETFL, 0) >= 0) ++used;
    }
    errno = saved_errno;
    return used;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    usb_audio_format_t fmt;
    char json[1280 + 11 * CONFIG_UAC_FREQ_NUM_MAX + 832 + 160 + 400 + 2 * RIGCTL_LINE_SIZE + 736];
    int n;
    if (usb_audio_get_format(&fmt)) {
        n = snprintf(json, sizeof(json),
                     "{\"name\":\"" NETWORK_REMOTE_NAME "\",\"streaming\":true,\"sample_rate\":%lu,\"channels\":%u,\"bits\":%u,\"bytes_per_sample\":%u,\"version\":\"%s\",\"build\":\"%s\"",
                     (unsigned long)fmt.sample_rate, fmt.channels, fmt.bit_resolution, fmt.subframe_size,
                     FTX1_VERSION, build_info_get());
    } else {
        n = snprintf(json, sizeof(json), "{\"name\":\"" NETWORK_REMOTE_NAME "\",\"streaming\":false,\"version\":\"%s\",\"build\":\"%s\"", FTX1_VERSION, build_info_get());
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        acc_scope_stats_t acc;
        acc_scope_get_stats(&acc);
        char acc_reason[2 * sizeof(acc.reason) + 1];
        json_escape(acc.reason, acc_reason, sizeof(acc_reason));
        const uint64_t usb_in_dhz = usb_input_rate_dhz();
        wifi_ap_record_t ap;
        const int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        // heap_* include PSRAM when it is enabled; int_* are the internal RAM only
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"cert_covers_ip\":%s,\"cert_sha256\":\"%s\","
                      "\"boot_id\":%lu,\"uptime_s\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,\"heap_block\":%lu,"
                      "\"int_free\":%lu,\"int_min\":%lu,\"int_block\":%lu,\"rssi\":%d,"
                      "\"conn_opened\":%u,\"conn_closed\":%u,\"tls_handshake_errors\":%u,"
                      "\"stream\":{\"active\":%s,\"starts\":%lu,\"chunks\":%lu,\"usb_timeouts\":%lu,\"slow_sends\":%lu,\"max_send_ms\":%lu,"
                      "\"buf_ms\":%lu,\"buf_max_ms\":%lu,\"overflows\":%lu,\"rate\":%lu,\"usb_in_hz\":%lu.%lu,"
                      "\"usb_bus_resets\":%lu,\"rx_paused\":%s},"
                      "\"acc\":{\"enabled\":%s,\"fps\":%lu,\"frames\":%lu,\"accepted\":%lu,\"rejected\":%lu,\"vfo_range_tx\":%lu,"
                      "\"age_ms\":%ld,\"last_len\":%lu,\"last_reject\":\"%s\","
                      "\"rej\":{\"length\":%lu,\"not_ftdx10\":%lu,\"vfo_copies\":%lu,\"vfo_range\":%lu,"
                      "\"span\":%lu,\"cat\":%lu},\"clients\":%lu}",
                      https_identity_covers_ip() ? "true" : "false", https_identity_fingerprint(),
                      (unsigned long)s_boot_id, (unsigned long)(esp_timer_get_time() / 1000000),
                      (unsigned long)esp_get_free_heap_size(),
                      (unsigned long)esp_get_minimum_free_heap_size(),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      rssi,
                      atomic_load(&s_tls_opened), atomic_load(&s_tls_closed), atomic_load(&s_tls_handshake_errors),
                      atomic_load(&s_stream_active) ? "true" : "false",
                      (unsigned long)s_st_starts, (unsigned long)s_st_chunks,
                      (unsigned long)s_st_usb_timeouts, (unsigned long)s_st_slow_sends,
                      (unsigned long)s_st_max_send_ms,
                      (unsigned long)s_st_buf_ms, (unsigned long)s_st_buf_max_ms, (unsigned long)s_st_overflows,
                      (unsigned long)s_st_rate, (unsigned long)(usb_in_dhz / 10), (unsigned long)(usb_in_dhz % 10),
                      (unsigned long)usb_audio_bus_resets(), usb_audio_rx_paused() ? "true" : "false",
                      acc.enabled ? "true" : "false", (unsigned long)acc.fps, (unsigned long)acc.frames,
                      (unsigned long)acc.accepted, (unsigned long)acc.rejected, (unsigned long)acc.vfo_range_tx,
                      acc.age_ms == UINT32_MAX ? -1L : (long)acc.age_ms, (unsigned long)acc.last_len, acc_reason,
                      (unsigned long)acc.rej[1], (unsigned long)acc.rej[2], (unsigned long)acc.rej[3],
                      (unsigned long)acc.rej[4], (unsigned long)acc.rej[5], (unsigned long)acc.rej[6],
                      (unsigned long)atomic_load(&s_scope_clients));
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        usb_audio_output_format_t output;
        const bool present = usb_audio_get_output_format(&output);
        char rates[11 * CONFIG_UAC_FREQ_NUM_MAX + 1] = {0};
        size_t used = 0;
        for (unsigned i = 0; i < output.rate_count; i++) {
            used += snprintf(rates + used, sizeof(rates) - used, "%s%lu",
                             i ? "," : "", (unsigned long)output.rates[i]);
        }
        char range[48] = {0};
        if (output.rate_max > 0) {
            snprintf(range, sizeof(range), ",\"rate_min\":%lu,\"rate_max\":%lu",
                     (unsigned long)output.rate_min, (unsigned long)output.rate_max);
        }
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"tx_audio\":{\"present\":%s,\"rates\":[%s],\"channels\":%u,\"bits\":%u,\"mps\":%u%s}}",
                      present ? "true" : "false", rates, output.channels, output.bits, output.mps, range);
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        const tx_state_t tx = tx_get_state();
        const usb_tx_stats_t audio = usb_audio_tx_stats();
        const usb_tx_transfer_stats_t transfers = usb_audio_tx_transfer_stats();
        const char *phase = tx.phase == TX_KEYED ? "KEYED" : tx.phase == TX_ARMED ? "ARMED" : "IDLE";
        char reason[32], swr[16], po[8];
        if (tx.swr_valid) {
            snprintf(swr, sizeof(swr), "%u.%02u", tx.swr / 100, tx.swr % 100);
        } else {
            strcpy(swr, "null");
        }
        if (tx.po_valid) {
            snprintf(po, sizeof(po), "%u", tx.po_raw);
        } else {
            strcpy(po, "null");
        }
        n--;
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"tx\":{\"state\":\"%s\",\"owner\":%lu,\"left_s\":%u,\"reason\":\"%s\","
                      "\"stopping\":%s,\"rx_confirmed\":%s,\"off_attempts\":%u,\"off_error\":%d,"
                      "\"audio_rate\":%u,\"underruns\":%u,\"written_bytes\":%u,"
                      "\"completed_bytes\":%u,\"completed_packets\":%u,\"failed_packets\":%u,\"pcm_peak\":%u,\"submit_error\":%d,"
                      "\"swr\":%s,\"swr_raw\":%u,\"po\":%s,\"po_raw\":%u}}",
                      phase, (unsigned long)tx.owner, tx.left_s, tx_reason(&tx, reason),
                      tx.stopping ? "true" : "false", tx.rx_confirmed ? "true" : "false", tx.off_attempts, tx.off_error,
                      audio.audio_rate, audio.underruns, audio.written_bytes,
                      transfers.completed_bytes, transfers.completed_packets, transfers.failed_packets,
                      transfers.pcm_peak, transfers.submit_error,
                      swr, tx.swr_raw, po, tx.po_raw);
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        const cat_write_stats_t cat = cat_get_write_stats();
        n--;
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"cat_rejected_commands\":%u,\"cat_write\":{\"budget_ms\":%u,\"keyed_poll_budget_ms\":%u,"
                      "\"all\":{\"calls\":%u,\"timeouts\":%u,\"over_budget\":%u,\"max_us\":%u},"
                      "\"keyed\":{\"calls\":%u,\"timeouts\":%u,\"over_budget\":%u,\"max_us\":%u},"
                      "\"meters\":{\"calls\":%u,\"timeouts\":%u,\"over_budget\":%u,\"max_us\":%u}}}",
                      cat_rejected_commands(), cat.budget_ms, cat.keyed_poll_budget_ms, cat.all.calls, cat.all.timeouts, cat.all.over_budget, cat.all.max_us,
                      cat.keyed.calls, cat.keyed.timeouts, cat.keyed.over_budget, cat.keyed.max_us,
                      cat.meters.calls, cat.meters.timeouts, cat.meters.over_budget, cat.meters.max_us);
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        const ft8_tx_status_t ft8 = ft8_tx_status(0);
        const ft8_audio_stats_t audio = usb_audio_ft8_stats();
        n--;
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"ft8_tx\":{\"armed\":%s,\"remaining\":%u,\"clock\":\"%s\","
                      "\"age_ms\":%" PRId64 ",\"offset_ms\":%" PRId64 ",\"rtt_ms\":%u,\"usb_lead_ms\":%u,"
                      "\"preparing\":%s,\"prepare_ms\":%u,\"reason\":\"%s\",\"audio\":{\"max_task_gap_us\":%u,\"late_threshold_us\":%u",
                      ft8.armed ? "true" : "false", ft8.remaining, ft8.clock, ft8.age_ms,
                      ft8.offset_ms, ft8.rtt_ms, ft8.lead_ms,
                      ft8.preparing ? "true" : "false", ft8.prepare_ms, ft8.reason, audio.max_task_gap_us, USB_FT8_LATE_GAP_US);
        const struct { const char *name; ft8_audio_event_t event; } events[] = {
            {"task_late", audio.task_late}, {"write_timeout", audio.write_timeout},
            {"write_failed", audio.write_failed}, {"iso_failed", audio.iso_failed},
            {"iso_short", audio.iso_short}, {"queue_empty", audio.queue_empty},
        };
        for (size_t i = 0; i < sizeof(events) / sizeof(*events) && n > 0 && (size_t)n < sizeof(json); ++i) {
            const ft8_audio_event_t event = events[i].event;
            n += snprintf(json + n, sizeof(json) - n,
                          ",\"%s\":{\"count\":%u,\"first_ms\":%d,\"last_ms\":%d}",
                          events[i].name, event.count, event.count ? event.first_ms : -1,
                          event.count ? event.last_ms : -1);
        }
        if (n > 0 && (size_t)n < sizeof(json)) {
            n += snprintf(json + n, sizeof(json) - n, "}}}");
        }
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        const tx_voice_stats_t voice = tx_voice_stats();
        n--;
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"tx_voice\":{\"level_ms\":%u,\"target_ms\":%u,\"underruns\":%u,"
                      "\"dropped_frames\":%u,\"rate_ppm\":%d}}",
                      voice.level_ms, voice.target_ms, voice.underruns, voice.dropped_frames, voice.rate_ppm);
    }
    if (n > 0 && (size_t)n < sizeof(json)) {
        const rigctld_stats_t rigctl = rigctld_stats();
        char last_rejected[2 * sizeof(rigctl.last_rejected) + 1];
        json_escape(rigctl.last_rejected, last_rejected, sizeof(last_rejected));
        n--;
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"lwip\":{\"sockets_used\":%u,\"socket_limit\":%u},"
                      "\"rigctld\":{\"enabled\":%s,\"listening\":%s,\"port\":%u,"
                      "\"clients\":%u,\"commands\":%u,\"rejected\":%u,\"debug\":%s,\"last_rejected\":\"%s\","
                      "\"sockets_used\":%u,\"opened\":%u,\"closed\":%u,\"accept_errors\":%u,"
                      "\"send_errors\":%u,\"send_timeouts\":%u}}",
                      lwip_sockets_used(), (unsigned)CONFIG_LWIP_MAX_SOCKETS,
                      rigctl.enabled ? "true" : "false", rigctl.listening ? "true" : "false",
                      rigctl.port, rigctl.clients, rigctl.commands, rigctl.rejected,
                      rigctl.debug ? "true" : "false", last_rejected,
                      rigctl.clients + rigctl.listening, rigctl.opened, rigctl.closed,
                      rigctl.accept_errors, rigctl.send_errors, rigctl.send_timeouts);
    }
    return send_json(req, json, n, sizeof(json));
}

static esp_err_t cat_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    cat_state_t st;
    char note[2 * sizeof(st.last_error) + 1], write_error[2 * sizeof(st.write_error) + 1];
    char json[CAT_JSON_BYTES];
    cat_get_state(&st);
    json_escape(st.write_error, write_error, sizeof(write_error));
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
             "\"age_ms\":%lu,\"devices\":%u,\"cp210x\":%u,\"answered\":%u,\"waiting_for_cat\":%s,\"cat_usb_resets\":%u,\"note\":\"%s\"}",
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
             (unsigned long)st.age_ms, st.devices, st.cp210x, st.answered,
             st.waiting_for_cat ? "true" : "false", st.cat_usb_resets, note);
    if (n > 0 && (size_t)n < sizeof(json)) {
        const char *rx_vfo = st.connected && st.rx_vfo_known
                           ? (st.rx_vfo_sub ? "\"sub\"" : "\"main\"") : "null";
        const char *tx_vfo = st.connected && st.tx_vfo == CAT_TX_VFO_MAIN ? "\"main\"" :
                             st.connected && st.tx_vfo == CAT_TX_VFO_SUB ? "\"sub\"" : "null";
        n--;
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"tx_voice_source\":%u,\"tx_enabled\":%s,\"tx_cat\":%s,\"tx_radio\":%s,\"tx_radio_known\":%s,"
                      "\"rx_vfo\":%s,\"tx_vfo\":%s,\"write_error_seq\":%lu,\"write_error\":\"%s\"}",
                      (unsigned)st.voice_source, app_config_tx_enabled() ? "true" : "false",
                      st.tx_cat ? "true" : "false", st.tx_radio ? "true" : "false",
                      st.tx_radio_known ? "true" : "false", rx_vfo, tx_vfo, (unsigned long)st.write_error_seq, write_error);
    }
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

typedef struct {
    uint32_t owner;
} tx_ws_session_t;

typedef struct {
    char type[12];
    char pass[APP_CONFIG_PASS_MAX_LEN + 1];
    char message[FT8_TX_TEXT_SIZE];
    int64_t offset;
    unsigned hz, rtt;
    bool on, odd;
    tx_tone_t tone;
    int dbfs;
} tx_ws_message_t;

static bool parse_tx_password(const char **p, char out[APP_CONFIG_PASS_MAX_LEN + 1])
{
    if (**p != '"') {
        return false;
    }
    ++*p;
    size_t n = 0;
    while (**p != '"') {
        unsigned char c = (unsigned char)**p;
        if (c < 0x20 || c > 0x7e || n == APP_CONFIG_PASS_MAX_LEN) {
            return false;
        }
        ++*p;
        if (c == '\\') {
            c = (unsigned char)**p;
            if (c != '\\' && c != '"') {
                return false;
            }
            ++*p;
        }
        out[n++] = (char)c;
    }
    ++*p;
    return true;
}

static bool parse_tx_message(const char *p, tx_ws_message_t *msg)
{
    skip_ws(&p);
    if (*p++ != '{') {
        return false;
    }
    enum { TYPE = 1, PASS = 2, ON = 4, TONE = 8, DBFS = 16,
           MESSAGE = 32, HZ = 64, ODD = 128, OFFSET = 256, RTT = 512 };
    msg->dbfs = TX_TONE_DEFAULT_DBFS;
    unsigned fields = 0;
    for (;;) {
        char key[8];
        skip_ws(&p);
        if (!parse_string(&p, key, sizeof(key))) {
            return false;
        }
        skip_ws(&p);
        if (*p++ != ':') {
            return false;
        }
        skip_ws(&p);
        unsigned field;
        if (strcmp(key, "t") == 0) {
            field = TYPE;
            if (!parse_string(&p, msg->type, sizeof(msg->type))) {
                return false;
            }
        } else if (strcmp(key, "pass") == 0) {
            field = PASS;
            if (!parse_tx_password(&p, msg->pass)) {
                return false;
            }
        } else if (strcmp(key, "on") == 0) {
            field = ON;
            char token[8];
            if (!parse_token(&p, token, sizeof(token)) ||
                (strcmp(token, "true") != 0 && strcmp(token, "false") != 0)) {
                return false;
            }
            msg->on = strcmp(token, "true") == 0;
        } else if (strcmp(key, "tone") == 0) {
            field = TONE;
            char tone[8];
            if (!parse_string(&p, tone, sizeof(tone))) {
                return false;
            }
            if (strcmp(tone, "single") == 0) {
                msg->tone = TX_TONE_SINGLE;
            } else if (strcmp(tone, "dual") == 0) {
                msg->tone = TX_TONE_DUAL;
            } else if (strcmp(tone, "voice") == 0) {
                msg->tone = TX_TONE_VOICE;
            } else if (strcmp(tone, "off") != 0) {
                return false;
            }
        } else if (strcmp(key, "msg") == 0) {
            field = MESSAGE;
            if (!parse_string(&p, msg->message, sizeof(msg->message))) return false;
        } else if (strcmp(key, "hz") == 0 || strcmp(key, "rtt") == 0 || strcmp(key, "offset") == 0) {
            char token[16], *end;
            field = strcmp(key, "hz") == 0 ? HZ : strcmp(key, "rtt") == 0 ? RTT : OFFSET;
            if (!parse_token(&p, token, sizeof(token)) || token[0] < '0' || token[0] > '9') return false;
            long long value = strtoll(token, &end, 10);
            if (*end || (field != OFFSET && value > 3000)) return false;
            if (field == HZ) msg->hz = (unsigned)value;
            else if (field == RTT) msg->rtt = (unsigned)value;
            else msg->offset = value;
        } else if (strcmp(key, "odd") == 0) {
            field = ODD;
            char token[8];
            if (!parse_token(&p, token, sizeof(token)) ||
                (strcmp(token, "true") != 0 && strcmp(token, "false") != 0)) return false;
            msg->odd = strcmp(token, "true") == 0;
        } else if (strcmp(key, "dbfs") == 0) {
            field = DBFS;
            char token[5], *end;
            if (!parse_token(&p, token, sizeof(token)) || token[0] != '-' ||
                token[1] < '1' || token[1] > '9') {
                return false;
            }
            const long level = strtol(token, &end, 10);
            if (*end || level < TX_TONE_MIN_DBFS || level > TX_TONE_MAX_DBFS) {
                return false;
            }
            msg->dbfs = (int)level;
        } else {
            return false;
        }
        if (fields & field) {
            return false;
        }
        fields |= field;
        skip_ws(&p);
        if (*p == '}') {
            ++p;
            break;
        }
        if (*p++ != ',') {
            return false;
        }
    }
    skip_ws(&p);
    return *p == '\0' && ((fields == (TYPE | PASS) && strcmp(msg->type, "tx_auth") == 0) ||
                          ((fields & ~(TONE | DBFS)) == (TYPE | ON) &&
                           (!(fields & (TONE | DBFS)) || (msg->on && (fields & TONE))) && strcmp(msg->type, "ptt") == 0) ||
                          ((fields & ~DBFS) == (TYPE | MESSAGE | HZ | ODD) && strcmp(msg->type, "ft8_arm") == 0) ||
                          (fields == (TYPE | DBFS) && strcmp(msg->type, "ft8_level") == 0) ||
                          (fields == (TYPE | OFFSET | RTT) && strcmp(msg->type, "ft8_clock") == 0) ||
                          (fields == TYPE && (strcmp(msg->type, "hb") == 0 || strcmp(msg->type, "status") == 0 ||
                                              strcmp(msg->type, "ft8_ping") == 0 || strcmp(msg->type, "ft8_halt") == 0)));
}

static void free_tx_session(void *ctx)
{
    tx_ws_session_t *session = ctx;
    ft8_tx_halt(session->owner, "Client disconnected");
    tx_release(session->owner, "Client disconnected");
    free(session);
}

static esp_err_t tx_ws_pre_handshake(httpd_req_t *req)
{
    return web_auth_ws_check(req) ? ESP_OK : ESP_FAIL;
}

static esp_err_t tx_ws_handler(httpd_req_t *req)
{
    static uint32_t next_owner;
    tx_ws_session_t *session = req->free_ctx == free_tx_session ? req->sess_ctx : NULL;
    httpd_ws_frame_t frame = {0};
    uint8_t input[TX_VOICE_PACKET_BYTES];
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
    if (frame.type == HTTPD_WS_TYPE_BINARY) {
        if (!session || !frame.final || !tx_voice_packet_size_valid(frame.len)) return ESP_FAIL;
        frame.payload = input;
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) return ESP_FAIL;
        if (!tx_submit_voice(session->owner, input, frame.len)) return ESP_FAIL;
        // A burst of pending TLS records can keep httpd continuously runnable.
        // Block for one tick so lower priority tasks, including IDLE0, can run.
        vTaskDelay(1);
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || !frame.final || frame.len == 0 || frame.len >= 192) {
        ESP_LOGW(TAG, "TX WebSocket rejected: invalid control frame");
        return ESP_FAIL;
    }
    frame.payload = input;
    if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK || memchr(input, 0, frame.len)) {
        return ESP_FAIL;
    }
    input[frame.len] = 0;

    tx_ws_message_t msg = {0};
    if (!parse_tx_message((const char *)input, &msg)) {
        ESP_LOGW(TAG, "TX WebSocket rejected: invalid control message");
        return ESP_FAIL;
    }
    const char *error = "";
    if (strcmp(msg.type, "tx_auth") == 0) {
        if (session) {
            return ESP_FAIL;  // authenticate once per connection
        }
        if (!web_auth_tx_check(req, msg.pass)) {
            error = "TX authorization refused";
        } else {
            session = calloc(1, sizeof(*session));
            if (!session) {
                return ESP_ERR_NO_MEM;
            }
            if (++next_owner == 0) {
                ++next_owner;
            }
            session->owner = next_owner;
            req->sess_ctx = session;
            req->free_ctx = free_tx_session;
            ESP_LOGI(TAG, "TX WebSocket authorized");
        }
    } else if (!session) {
        return ESP_FAIL;
    } else if (strcmp(msg.type, "hb") == 0) {
        ft8_tx_heartbeat(session->owner);
        tx_heartbeat(session->owner);
    } else if (strcmp(msg.type, "ft8_arm") == 0) {
        error = ft8_tx_arm(session->owner, msg.message, msg.hz, msg.odd, msg.dbfs);
    } else if (strcmp(msg.type, "ft8_level") == 0) {
        ft8_tx_set_level(session->owner, msg.dbfs);
    } else if (strcmp(msg.type, "ft8_halt") == 0) {
        ft8_tx_halt(session->owner, "FT8 halted");
    } else if (strcmp(msg.type, "ft8_clock") == 0) {
        ft8_tx_browser_clock(session->owner, msg.offset, msg.rtt);
    } else if (strcmp(msg.type, "ft8_ping") == 0) {
        char pong[64];
        const int n = snprintf(pong, sizeof(pong), "{\"t\":\"ft8_pong\",\"mono\":%" PRId64 "}",
                               esp_timer_get_time() / 1000);
        frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)pong, .len = n};
        return httpd_ws_send_frame(req, &frame);
    } else if (strcmp(msg.type, "status") == 0) {
        // Inspect state without extending the heartbeat.
    } else if (!msg.on) {
        ft8_tx_halt(session->owner, "FT8 halted");
        tx_release(session->owner, "PTT released");
    } else if (!tx_acquire(session->owner)) {
        error = "TX busy or unavailable";
    } else if (!tx_key_tone(session->owner, msg.tone, msg.dbfs)) {
        const tx_state_t refused = tx_get_state();
        error = refused.reason[0] ? refused.reason : "PTT unavailable";
    }

    const tx_state_t state = tx_get_state();
    cat_state_t cat;
    cat_get_state(&cat);
    const char *tx_vfo = cat.tx_vfo == CAT_TX_VFO_MAIN ? "main" :
                         cat.tx_vfo == CAT_TX_VFO_SUB ? "sub" : "";
    const char *phase = state.phase == TX_KEYED ? "KEYED" : state.phase == TX_ARMED ? "ARMED" : "IDLE";
    const ft8_tx_status_t ft8 = ft8_tx_status(session ? session->owner : 0);
    char json[768], reason[32];
    const int n = snprintf(json, sizeof(json),
                           "{\"t\":\"tx\",\"state\":\"%s\",\"left_s\":%u,\"reason\":\"%s\","
                           "\"on_air\":%s,\"stopping\":%s,\"rx_confirmed\":%s,\"rx_paused\":%s,\"tx_vfo\":\"%s\",\"auth\":%s,\"owner\":%s,\"error\":\"%s\","
                           "\"swr_x100\":%u,\"swr_raw\":%u,\"po_raw\":%u,\"po_valid\":%s,"
                           "\"alc_raw\":%u,\"alc_valid\":%s, "
                           "\"ft8_owned\":%s,\"ft8_preparing\":%s,\"ft8_remaining\":%u,\"ft8_reason\":\"%s\",\"ft8_clock\":\"%s\"}",
                           phase, state.left_s, tx_reason(&state, reason), state.on_air ? "true" : "false",
                           state.stopping ? "true" : "false", state.rx_confirmed ? "true" : "false",
                           usb_audio_rx_paused() ? "true" : "false", tx_vfo,
                           session ? "true" : "false", session && state.owner == session->owner ? "true" : "false",
                           error, state.swr, state.swr_raw, state.po_raw, state.po_valid ? "true" : "false",
                           state.alc_raw, state.alc_valid ? "true" : "false",
                           ft8.owned ? "true" : "false", ft8.preparing ? "true" : "false",
                           ft8.remaining, ft8.reason, ft8.clock);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return ESP_FAIL;
    }
    frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json, .len = n};
    return httpd_ws_send_frame(req, &frame);
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

#define UI_PREFS_NAMESPACE "ui_prefs"
#define UI_PREFS_KEY "settings"

static esp_err_t prefs_load(ui_prefs_t *settings, char *json, size_t capacity)
{
    *settings = (ui_prefs_t){0};
    nvs_handle_t handle;
    esp_err_t err = nvs_open(UI_PREFS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_get_str(handle, UI_PREFS_KEY, json, &capacity);
    nvs_close(handle);
    if (err == ESP_OK) {
        bool reset;
        if (!ui_prefs_parse(json, settings, &reset) || reset) *settings = (ui_prefs_t){0};
    } else if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH) {
        err = ESP_OK;
    }
    return err;
}

static esp_err_t prefs_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) return ESP_OK;
    ui_prefs_t settings;
    char json[UI_PREFS_JSON_BYTES];
    const int n = prefs_load(&settings, json, sizeof(json)) == ESP_OK
        ? ui_prefs_format(&settings, json, sizeof(json)) : -1;
    return send_json(req, json, n, sizeof(json));
}

static esp_err_t prefs_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, true)) return ESP_OK;
    web_auth_set_common_headers(req);
    if (req->content_len >= UI_PREFS_JSON_BYTES) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Request body is too large");
    }
    char json[UI_PREFS_JSON_BYTES];
    ui_prefs_t patch, settings;
    bool reset;
    if (!content_type_is_json(req) || !read_body(req, json, sizeof(json))) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    if (!ui_prefs_parse(json, &patch, &reset)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid UI preference");
    }

    // The HTTP server serializes these handlers. Merge only the fields changed by this client.
    esp_err_t err = reset ? ESP_OK : prefs_load(&settings, json, sizeof(json));
    if (err == ESP_OK && !reset) {
        const ui_prefs_t before = settings;
        ui_prefs_merge(&settings, &patch);
        if (memcmp(&settings, &before, sizeof(settings)) == 0) return send_json(req, "{}", 2, 3);
        if (ui_prefs_format(&settings, json, sizeof(json)) < 0) err = ESP_FAIL;
    }
    nvs_handle_t handle;
    if (err == ESP_OK) err = nvs_open(UI_PREFS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = reset ? nvs_erase_key(handle, UI_PREFS_KEY) : nvs_set_str(handle, UI_PREFS_KEY, json);
        if (reset && err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return send_json(req, "{}", err == ESP_OK ? 2 : -1, 3);
}

static esp_err_t cat_post_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, true)) {
        return ESP_OK;
    }
    web_auth_set_common_headers(req);
    // Close on a rejected or incomplete body. A fully consumed POST can reuse the session.
    if (!content_type_is_json(req)) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Type must be application/json");
    }
    if (req->content_len >= CAT_POST_MAX_BYTES) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Request body is too large");
    }

    char buf[CAT_POST_MAX_BYTES];
    cat_req_t r;
    if (!read_body(req, buf, sizeof(buf))) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    }
    if (!parse_cat_request(buf, &r)) {
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

// GET /catlog?since=N: the CAT traffic newer than entry N, oldest first, at most CATLOG_MAX per answer.
// {"seq":<last entry sent>,"now":<ms since boot>,"e":[[seq,ms,"T"|"R",kind,"text"],...]}
#define CATLOG_MAX 64
static esp_err_t catlog_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    uint32_t since = 0;
    char query[32], value[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
        since = (uint32_t)strtoul(value, NULL, 10);
    }
    const size_t cap = 64 + CATLOG_MAX * (48 + 2 * CAT_LOG_TEXT_LEN);
    cat_log_entry_t *entries = heap_caps_malloc(CATLOG_MAX * sizeof(cat_log_entry_t), MALLOC_CAP_8BIT);
    char *json = heap_caps_malloc(cap, MALLOC_CAP_8BIT);
    if (entries == NULL || json == NULL) {
        free(entries);
        free(json);
        web_auth_set_common_headers(req);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    const size_t n = cat_log_read(since, entries, CATLOG_MAX);
    const uint32_t last = n ? entries[n - 1].seq : since;
    int len = snprintf(json, cap, "{\"seq\":%lu,\"now\":%lu,\"e\":[", (unsigned long)last,
                       (unsigned long)(esp_timer_get_time() / 1000));
    for (size_t i = 0; i < n && len > 0 && (size_t)len < cap; i++) {
        char text[2 * CAT_LOG_TEXT_LEN + 1];
        json_escape(entries[i].text, text, sizeof(text));   // bytes echoed by the radio
        len += snprintf(json + len, cap - len, "%s[%lu,%lu,\"%c\",%u,\"%s\"]", i ? "," : "",
                        (unsigned long)entries[i].seq, (unsigned long)entries[i].ms,
                        entries[i].dir == 'R' ? 'R' : 'T', (unsigned)entries[i].kind, text);
    }
    if (len > 0 && (size_t)len < cap) {
        len += snprintf(json + len, cap - len, "]}");
    }
    const esp_err_t err = send_json(req, json, len, cap);
    free(entries);
    free(json);
    return err;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, true)) {
        return ESP_OK;
    }
    if (!tx_quiesce("Reboot")) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Waiting for radio RX. Retry reboot.");
    }
    web_auth_set_common_headers(req);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "Reboot requested via web API");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    usb_audio_prepare_restart();
    esp_restart();
    return ESP_OK;
}

// ---- GET /scope.bin: the FTDX10's ACC scope rows as a binary stream ------------------------------------
// One record per accepted frame (up to 25 a second, ?fps=N for fewer), all little-endian:
//   u8 0xAC, u8 version 1, u8 placement (0 Center 1 Cursor 2 Fix), u8 scope mode byte,
//   u32 seq, u32 vfo_hz, u32 center_hz, u32 span_hz, u32 marker_hz, u16 points, u16 0,
//   then `points` levels (0-255, higher is stronger). points = 0 is a keep-alive sent about once a second
//   while no frame arrives, so a vanished client is noticed.
#define SCOPE_REC_HDR      28
#define SCOPE_MAX_CLIENTS  2

typedef struct {
    httpd_req_t *req;
    uint32_t min_interval_ms;
    int64_t requested_ms;
} scope_ctx_t;

static void put_scope_header(uint8_t *h, const acc_scope_row_t *r, uint16_t points)
{
    h[0] = 0xAC;
    h[1] = 1;
    h[2] = r ? r->placement : 3;
    h[3] = r ? r->scope_mode : 0;
    put_le32(h + 4, r ? r->seq : 0);
    put_le32(h + 8, r ? r->vfo_hz : 0);
    put_le32(h + 12, r ? r->center_hz : 0);
    put_le32(h + 16, r ? r->span_hz : 0);
    put_le32(h + 20, r ? r->marker_hz : 0);
    put_le16(h + 24, points);
    put_le16(h + 26, 0);
}

static void scope_task(void *arg)
{
    scope_ctx_t *sc = (scope_ctx_t *)arg;
    httpd_req_t *req = sc->req;
    acc_scope_row_t *row = heap_caps_malloc(sizeof(*row), MALLOC_CAP_8BIT);
    uint8_t *rec = heap_caps_malloc(SCOPE_REC_HDR + ACC_SCOPE_POINTS, MALLOC_CAP_8BIT);
    esp_err_t err = (row && rec) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        web_auth_set_common_headers(req);
    }
    uint32_t last_seq = 0;
    int64_t last_sent_ms = 0;
    int64_t last_any_ms = sc->requested_ms - 1000; // send headers on the first iteration, even without a frame
    while (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const int64_t now = esp_timer_get_time() / 1000;
        const uint32_t seq = acc_scope_seq();
        if (seq != last_seq && seq != 0 && now - last_sent_ms >= sc->min_interval_ms && acc_scope_get_row(row)) {
            const bool first = last_seq == 0;
            last_seq = row->seq;
            last_sent_ms = now;
            last_any_ms = now;
            put_scope_header(rec, row, ACC_SCOPE_POINTS);
            memcpy(rec + SCOPE_REC_HDR, row->level, ACC_SCOPE_POINTS);
            err = httpd_resp_send_chunk(req, (const char *)rec, SCOPE_REC_HDR + ACC_SCOPE_POINTS);
            if (first && err == ESP_OK) {
                ESP_LOGI(TAG, "Scope first frame sent in %lld ms, fd=%d",
                         (long long)(esp_timer_get_time() / 1000 - sc->requested_ms),
                         httpd_req_to_sockfd(req));
            }
        } else if (now - last_any_ms >= 1000) {
            last_any_ms = now;
            put_scope_header(rec, NULL, 0);
            err = httpd_resp_send_chunk(req, (const char *)rec, SCOPE_REC_HDR);
        }
    }
    free(row);
    free(rec);
    ESP_LOGI(TAG, "Scope ended (%s), fd=%d", esp_err_to_name(err), httpd_req_to_sockfd(req));
    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    free(sc);
    atomic_fetch_sub(&s_scope_clients, 1);
    vTaskDelete(NULL);
}

static esp_err_t scope_handler(httpd_req_t *req)
{
    if (!web_auth_check(req, WEB_AUTH_REALM_WEB, false)) {
        return ESP_OK;
    }
    uint32_t fps = 25;
    char query[32], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "fps", value, sizeof(value)) == ESP_OK) {
        const unsigned long v = strtoul(value, NULL, 10);
        if (v >= 1 && v <= 25) {
            fps = (uint32_t)v;
        }
    }
    if (atomic_fetch_add(&s_scope_clients, 1) >= SCOPE_MAX_CLIENTS) {
        atomic_fetch_sub(&s_scope_clients, 1);
        web_auth_set_common_headers(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Too many scope clients", HTTPD_RESP_USE_STRLEN);
    }
    scope_ctx_t *sc = calloc(1, sizeof(*sc));
    httpd_req_t *async_req = NULL;
    if (sc == NULL || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        free(sc);
        atomic_fetch_sub(&s_scope_clients, 1);
        return httpd_resp_send_500(req);
    }
    sc->req = async_req;
    sc->requested_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Scope requested, fd=%d", httpd_req_to_sockfd(req));
    sc->min_interval_ms = 1000 / fps - 5;      // a little slack, so 25 fps really gets every frame
    // Core 1 with the web server; TLS record encryption runs in this task, hence the stack
    if (xTaskCreatePinnedToCore(scope_task, "scope", 6144, sc, 5, NULL, WEB_CORE) != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        free(sc);
        atomic_fetch_sub(&s_scope_clients, 1);
        return ESP_FAIL;
    }
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

// ---- Plain HTTP on port 80: only a redirect to HTTPS ---------------------------------------------------
// Typing just the IP address (or trx-remote.local) makes a browser try http:// first, where nothing answered.
// This server answers every request with 301 to the same host over https, and does nothing else: no page,
// no data, no authentication (the HTTPS server asks for it).

/** A host name or IPv4 address as it may appear in a Host header, without the port */
static bool redirect_host_ok(const char *h)
{
    const size_t n = strlen(h);
    if (n == 0 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

/** Keep the path of the request if it is plain (so http://<ip>/map lands on /map), otherwise "/" */
static bool redirect_path_ok(const char *p)
{
    const size_t n = strlen(p);
    if (n == 0 || n > 64 || p[0] != '/' || (n > 1 && p[1] == '/')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    char host[72] = "";
    if (httpd_req_get_hdr_value_len(req, "Host") < sizeof(host)) {
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    }
    char *colon = strchr(host, ':');            // drop ":80"
    if (colon) {
        *colon = '\0';
    }
    if (!redirect_host_ok(host)) {
        // No usable Host header: use this device's own address
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use https://");
        }
        snprintf(host, sizeof(host), IPSTR, IP2STR(&ip.ip));
    }
    char path[72] = "/";
    const char *q = strchr(req->uri, '?');
    const size_t plen = q ? (size_t)(q - req->uri) : strlen(req->uri);
    if (plen < sizeof(path)) {
        memcpy(path, req->uri, plen);
        path[plen] = '\0';
        if (!redirect_path_ok(path)) {
            strcpy(path, "/");
        }
    }
    char location[160];
    snprintf(location, sizeof(location), "https://%s%s", host, path);
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

static void redirect_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT;    // HTTPS reserves ESP_HTTPD_DEF_CTRL_PORT + 1
    cfg.max_open_sockets = 2;
    cfg.max_uri_handlers = 2;
    cfg.lru_purge_enable = true;
    cfg.core_id = WEB_CORE;
    cfg.stack_size = 3072;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t h = NULL;
    const esp_err_t err = httpd_start(&h, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP redirect port %u (control %u): %s", cfg.server_port, cfg.ctrl_port, esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t any_get = {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler};
    static const httpd_uri_t any_head = {.uri = "/*", .method = HTTP_HEAD, .handler = redirect_handler};
    httpd_register_uri_handler(h, &any_get);
    httpd_register_uri_handler(h, &any_head);
    ESP_LOGI(TAG, "HTTP port 80 redirects to HTTPS");
}

esp_err_t web_server_start(void)
{
    s_boot_id = esp_random();
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.user_cb = count_tls_events;
    config.servercert = servercert_pem_start;
    config.servercert_len = servercert_pem_end - servercert_pem_start;
    config.prvtkey_pem = prvtkey_pem_start;
    config.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;
    // tls_version is left at ESP_TLS_VER_ANY: this mbedTLS build only provides TLS 1.2 and 1.3

    // Chrome (mobile/desktop) opens several parallel connections and TLS handshakes occupy
    // sockets until complete. The default of 4 fills up immediately, causing "Too many retries".
    // 8 is enough for a single browser client (page + /status poll + /cat poll + stream).
    config.httpd.max_open_sockets = 10;   // + the ACC scope stream (two browsers at most)
    // Keep HTTPS and lwIP off the core that services USB audio and ACC.
    config.httpd.core_id = WEB_CORE;
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
    ESP_RETURN_ON_ERROR(https_identity_start(&server, &config), TAG, "HTTPS server start");

    static const httpd_uri_t uris[] = {
        {.uri = "/",           .method = HTTP_GET,  .handler = index_handler},
        {.uri = "/tx_mic.js",  .method = HTTP_GET,  .handler = tx_mic_handler},
        {.uri = "/map",        .method = HTTP_GET,  .handler = map_handler},
        {.uri = "/status",     .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/prefs",      .method = HTTP_GET,  .handler = prefs_get_handler},
        {.uri = "/prefs",      .method = HTTP_POST, .handler = prefs_post_handler},
        {.uri = "/cat",        .method = HTTP_GET,  .handler = cat_handler},
        {.uri = "/cat",        .method = HTTP_POST, .handler = cat_post_handler},
        {.uri = "/reboot",     .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/catlog",     .method = HTTP_GET,  .handler = catlog_handler},
        {.uri = "/ws",         .method = HTTP_GET,  .handler = tx_ws_handler, .is_websocket = true,
         .ws_pre_handshake_cb = tx_ws_pre_handshake},
        {.uri = "/scope.bin",  .method = HTTP_GET,  .handler = scope_handler},
        {.uri = "/stream.pcm", .method = HTTP_GET,  .handler = pcm_handler},
        {.uri = "/stream.wav", .method = HTTP_GET,  .handler = wav_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register handler");
    }
    ESP_RETURN_ON_ERROR(ota_register(server), TAG, "register ota");
    ESP_LOGI(TAG, "HTTPS server started on port %d", config.port_secure);
    redirect_server_start();
    web_auth_log_startup();
    return ESP_OK;
}
