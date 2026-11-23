/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND Apache-2.0
 *
 * Attribution: the USB Host / UAC class driver start-up in this file (usb_lib_task(), the
 * uac_host_driver_config_t settings, and the task priority constants) follows the structure of the
 * "audio_player" example of the espressif/usb_host_uac component:
 *   https://components.espressif.com/components/espressif/usb_host_uac
 *   Copyright 2024-2025 Espressif Systems (Shanghai) CO LTD, licensed under the Apache License 2.0.
 * That part remains subject to the Apache License 2.0. See the NOTICE file at the repository root.
 */

/*
 * Receives the radio's audio. The FTX-1 shows up on USB as an audio input; this file starts the USB Host
 * UAC class driver, opens that device when it is plugged in, and makes the PCM data available to the
 * web server through usb_audio_read().
 */

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "soc/usb_dwc_struct.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/uac_host.h"
#include "esp_timer.h"
#include "tx.h"
#include "usb_audio.h"

// Above the stream's USB reader (6), the web server and LwIP (18), below the Wi-Fi driver (23): the
// isochronous transfers must be handed back to the controller on time or audio packets are lost.
// Both tasks only move small buffers, so they cost little CPU.
#define USB_HOST_TASK_PRIORITY  20
#define UAC_TASK_PRIORITY       20
#define MANAGER_TASK_PRIORITY   4
#define USB_CORE                1        // away from the Wi-Fi driver and LwIP on core 0
#define PREFERRED_BITS          16
#define MAX_LOCK_WAIT_MS        500
_Static_assert(CONFIG_AUDIO_RING_BUFFER_SIZE != USB_AUDIO_TX_RING_BYTES,
               "RX and TX rings must have distinct sizes for the UAC allocator");

// ESP32-S3 reports 200 FIFO lines in GHWCFG3.DFIFODEPTH. Use that budget
// rather than the 256 lines used to calculate the driver's default bias ratios.
// Keep RX at 104 lines, as in the balanced profile on a 200-line controller.
#define USB_FIFO_LINES          200
#define USB_NPTX_FIFO_LINES      32
#define USB_PTX_FIFO_LINES       64
#define USB_RX_FIFO_LINES       (USB_FIFO_LINES - USB_NPTX_FIFO_LINES - USB_PTX_FIFO_LINES)

// Match usb_dwc_hal_get_mps_limits(): RX reserves two status lines.
#define USB_RX_MPS_LIMIT        ((USB_RX_FIFO_LINES - 2) * 4)
#define USB_BULK_OUT_MPS_LIMIT  (USB_NPTX_FIFO_LINES * 4)
#define USB_ISOC_OUT_MPS_LIMIT  (USB_PTX_FIFO_LINES * 4)
_Static_assert(USB_RX_MPS_LIMIT >= 192 && USB_RX_MPS_LIMIT >= 64 &&
               USB_BULK_OUT_MPS_LIMIT >= 64 && USB_ISOC_OUT_MPS_LIMIT >= 192,
               "FIFO must fit 48 kHz stereo UAC and full-speed CDC endpoints");

#define TX_TASK_PRIORITY        18
#define TX_BLOCK_MS             10
#define TX_PREFILL_BLOCKS       4

typedef enum {
    EVT_RX_CONNECTED,
    EVT_TX_CONNECTED,
    EVT_DISCONNECTED,
} evt_type_t;

typedef struct {
    evt_type_t type;
    uint8_t addr;
    uint8_t iface_num;
    uac_host_device_handle_t handle;
} evt_t;

static const char *TAG = "usb_audio";

static QueueHandle_t s_evt_queue;
static SemaphoreHandle_t s_lock;               // protects input state and output descriptors
static uac_host_device_handle_t s_mic_handle;
static SemaphoreHandle_t s_output_lock;        // keeps handle open across UAC output operations
static uac_host_device_handle_t s_output_handle;
static uac_host_stream_config_t s_tx_format;
static TaskHandle_t s_tx_task;
static atomic_bool s_tx_available;
static atomic_bool s_tx_clean = true;
static atomic_bool s_tx_transfer_error;
static atomic_uint s_tx_request;               // generation, waveform and dBFS
static atomic_uint s_tx_rate, s_tx_underruns, s_tx_written_bytes;
static usb_audio_output_format_t s_output_format;
static usb_audio_format_t s_fmt;
static atomic_bool s_streaming;

static bool alt_supports_freq(const uac_host_dev_alt_param_t *p, uint32_t freq)
{
    if (p->sample_freq_type > 0) {
        for (int i = 0; i < p->sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
            if (p->sample_freq[i] == freq) {
                return true;
            }
        }
        return false;
    }
    return freq >= p->sample_freq_lower && freq <= p->sample_freq_upper;
}

/**
 * @brief Choose a stream format: prefer CONFIG_AUDIO_SAMPLE_RATE at 16 bit, otherwise the first alt setting
 */
static esp_err_t pick_stream_config(uac_host_device_handle_t h, uac_host_stream_config_t *cfg, uint8_t *subframe_size)
{
    uac_host_dev_info_t info;
    ESP_RETURN_ON_ERROR(uac_host_get_device_info(h, &info), TAG, "get device info");

    uac_host_dev_alt_param_t p;
    for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
        if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK) {
            continue;
        }
        if (p.bit_resolution == PREFERRED_BITS && alt_supports_freq(&p, CONFIG_AUDIO_SAMPLE_RATE)) {
            cfg->channels = p.channels;
            cfg->bit_resolution = p.bit_resolution;
            cfg->sample_freq = CONFIG_AUDIO_SAMPLE_RATE;
            *subframe_size = p.subframe_size;
            return ESP_OK;
        }
    }

    // Fallback: first alternate setting with its first supported frequency
    ESP_RETURN_ON_ERROR(uac_host_get_device_alt_param(h, 1, &p), TAG, "get alt param");
    cfg->channels = p.channels;
    cfg->bit_resolution = p.bit_resolution;
    cfg->sample_freq = p.sample_freq_type > 0 ? p.sample_freq[0] : p.sample_freq_lower;
    *subframe_size = p.subframe_size;
    ESP_LOGW(TAG, "%d Hz / %d bit not offered by device, falling back to %" PRIu32 " Hz / %u bit",
             CONFIG_AUDIO_SAMPLE_RATE, PREFERRED_BITS, cfg->sample_freq, cfg->bit_resolution);
    return ESP_OK;
}

static void post_event(evt_type_t type, uint8_t addr, uint8_t iface_num, uac_host_device_handle_t handle)
{
    evt_t evt = {.type = type, .addr = addr, .iface_num = iface_num, .handle = handle};
    // Called from USB driver context: must not block
    xQueueSend(s_evt_queue, &evt, 0);
}

static void uac_device_cb(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *arg)
{
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        post_event(EVT_DISCONNECTED, 0, 0, handle);
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        if (arg != NULL) {
            atomic_store(&s_tx_transfer_error, true);
        }
        ESP_LOGE(TAG, "Transfer error");
        break;
    default:
        // RX_DONE: data is pulled by usb_audio_read(), nothing to do here
        break;
    }
}

static void uac_driver_cb(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        post_event(EVT_RX_CONNECTED, addr, iface_num, NULL);
    } else if (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        post_event(EVT_TX_CONNECTED, addr, iface_num, NULL);
    }
}

static void handle_rx_connected(uint8_t addr, uint8_t iface_num)
{
    if (s_mic_handle != NULL) {
        ESP_LOGW(TAG, "An audio input is already in use, ignoring the new one");
        return;
    }

    const uac_host_device_config_t dev_config = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = CONFIG_AUDIO_RING_BUFFER_SIZE,
        .buffer_threshold = CONFIG_AUDIO_RING_BUFFER_SIZE / 2,
        .callback = uac_device_cb,
        .callback_arg = NULL,
    };
    uac_host_device_handle_t handle = NULL;
    if (uac_host_device_open(&dev_config, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open audio input (addr %u, iface %u)", addr, iface_num);
        return;
    }
    uac_host_printf_device_param(handle);

    uac_host_stream_config_t stream_cfg = {0};
    uint8_t subframe_size = 0;
    if (pick_stream_config(handle, &stream_cfg, &subframe_size) != ESP_OK ||
            uac_host_device_start(handle, &stream_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio input");
        uac_host_device_close(handle);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mic_handle = handle;
    s_fmt = (usb_audio_format_t) {
        .sample_rate = stream_cfg.sample_freq,
        .channels = stream_cfg.channels,
        .subframe_size = subframe_size,
        .bit_resolution = stream_cfg.bit_resolution,
    };
    s_streaming = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Streaming: %" PRIu32 " Hz, %u ch, %u bit (%u byte/sample)",
             s_fmt.sample_rate, s_fmt.channels, s_fmt.bit_resolution, s_fmt.subframe_size);
}

static void descriptor_client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    // The probe issues no transfers and does not use asynchronous events.
}

static uint16_t output_alt_mps(const usb_config_desc_t *config, uint8_t iface_num, unsigned alt_index)
{
    int offset;
    const usb_intf_desc_t *iface = usb_parse_interface_descriptor(config, iface_num, 0, &offset);
    for (unsigned i = 0; iface != NULL && i < alt_index; ++i) {
        iface = (const usb_intf_desc_t *)usb_parse_next_descriptor_of_type(
            (const usb_standard_desc_t *)iface, config->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset);
        if (iface != NULL && iface->bInterfaceNumber != iface_num) {
            return 0;
        }
    }
    // The streaming data endpoint precedes any explicit feedback endpoint.
    if (iface == NULL || iface->bNumEndpoints == 0) {
        return 0;
    }
    const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(iface, 0, config->wTotalLength, &offset);
    if (ep == NULL || USB_EP_DESC_GET_EP_DIR(ep) ||
        USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_ISOCHRONOUS) {
        return 0;
    }
    return USB_EP_DESC_GET_MPS(ep);
}

static bool pick_output_config(uac_host_device_handle_t handle, const usb_config_desc_t *config,
                               uint8_t iface_num, uac_host_stream_config_t *stream,
                               usb_audio_output_format_t *output)
{
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(handle, &info) != ESP_OK) {
        return false;
    }
    const uint32_t rates[] = {48000, 44100, 32000};
    for (uint8_t channels = 1; channels <= 2; ++channels) {
        for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
            uac_host_dev_alt_param_t selected = {0};
            unsigned selected_alt = 0;
            for (unsigned a = 1; a <= info.iface_alt_num; ++a) {
                uac_host_dev_alt_param_t fmt;
                if (uac_host_get_device_alt_param(handle, a, &fmt) != ESP_OK ||
                    fmt.channels != channels || fmt.bit_resolution != PREFERRED_BITS ||
                    !alt_supports_freq(&fmt, rates[r])) {
                    continue;
                }
                // Mirror uac_host_device_start(): last discrete match, or stop at a continuous match.
                selected = fmt;
                selected_alt = a;
                if (fmt.sample_freq_type == 0) {
                    break;
                }
            }
            if (selected_alt == 0 || selected.subframe_size != 2) {
                continue;
            }
            const uint16_t mps = output_alt_mps(config, iface_num, selected_alt);
            if (mps < rates[r] * channels * 2 / 1000 || mps > USB_ISOC_OUT_MPS_LIMIT) {
                continue;
            }
            *stream = (uac_host_stream_config_t){
                .channels = channels,
                .bit_resolution = PREFERRED_BITS,
                .sample_freq = rates[r],
            };
            *output = (usb_audio_output_format_t){
                .channels = channels,
                .bits = PREFERRED_BITS,
                .mps = mps,
            };
            if (selected.sample_freq_type == 0) {
                output->rate_min = selected.sample_freq_lower;
                output->rate_max = selected.sample_freq_upper;
            } else {
                output->rate_count = selected.sample_freq_type < UAC_FREQ_NUM_MAX
                                   ? selected.sample_freq_type : UAC_FREQ_NUM_MAX;
                memcpy(output->rates, selected.sample_freq, output->rate_count * sizeof(output->rates[0]));
            }
            return true;
        }
    }
    return false;
}

static bool probe_output_config(uac_host_device_handle_t handle, uint8_t addr, uint8_t iface_num,
                                uac_host_stream_config_t *stream, usb_audio_output_format_t *output)
{
    // UAC exposes alternate formats but not wMaxPacketSize. Read the cached
    // configuration using a short-lived host client without claiming the interface.
    const usb_host_client_config_t config = {
        .max_num_event_msg = 1,
        .async.client_event_callback = descriptor_client_event,
    };
    usb_host_client_handle_t client;
    if (usb_host_client_register(&config, &client) != ESP_OK) {
        return false;
    }
    bool selected = false;
    usb_device_handle_t device;
    if (usb_host_device_open(client, addr, &device) == ESP_OK) {
        const usb_config_desc_t *descriptor;
        if (usb_host_get_active_config_descriptor(device, &descriptor) == ESP_OK) {
            selected = pick_output_config(handle, descriptor, iface_num, stream, output);
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_device_close(client, device));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_client_deregister(client));
    return selected;
}

static void handle_output_connected(uint8_t addr, uint8_t iface_num)
{
    if (s_output_handle != NULL) {
        ESP_LOGW(TAG, "An audio output is already open, ignoring the new one");
        return;
    }

    const uac_host_device_config_t config = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = USB_AUDIO_TX_RING_BYTES,
        .callback = uac_device_cb,
        .callback_arg = &s_tx_available,  // marks this interface as output
    };
    uac_host_device_handle_t handle = NULL;
    esp_err_t err = uac_host_device_open(&config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open audio output (addr %u, iface %u): %s", addr, iface_num,
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Audio output (addr %u, iface %u), descriptor probe only", addr, iface_num);
    uac_host_printf_device_param(handle);
    uac_host_stream_config_t stream = {0};
    usb_audio_output_format_t output = {0};
    const bool selected = probe_output_config(handle, addr, iface_num, &stream, &output);
    if (selected) {
        ESP_LOGI(TAG, "Audio output selected: %" PRIu32 " Hz, %u ch, %u bit, endpoint MPS %u B",
                 stream.sample_freq, output.channels, output.bits, output.mps);
    } else {
        ESP_LOGW(TAG, "No supported 16-bit USB audio output format (addr %u, iface %u)", addr, iface_num);
    }

    xSemaphoreTake(s_output_lock, portMAX_DELAY);
    s_tx_format = stream;
    s_output_handle = handle;
    s_output_format = output;
    atomic_store(&s_tx_available, selected);
    xSemaphoreGive(s_output_lock);
}

static void handle_disconnected(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_output_lock, portMAX_DELAY);
    const bool output = handle == s_output_handle;
    if (output) {
        atomic_store(&s_tx_available, false);
        atomic_store(&s_tx_clean, true);
        s_output_handle = NULL;
        s_output_format = (usb_audio_output_format_t){0};
        uac_host_device_close(handle);
    }
    xSemaphoreGive(s_output_lock);
    if (output) {
        tx_unkey("USB audio output lost");
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (handle == s_mic_handle) {
            s_streaming = false;
            s_mic_handle = NULL;
        }
        xSemaphoreGive(s_lock);
        uac_host_device_close(handle);
    }
    ESP_LOGI(TAG, "Audio %s disconnected", output ? "output" : "input");
}

// ---- Recovery when the radio's audio does not come up ----------------------------------------------------
// Seen on the bench: after a reset of this board at an unlucky moment (right after flashing) CAT came back
// but the radio's USB audio did not, until a second reset. Here: when devices are on the bus but no audio
// input streams for RECOVER_AFTER_MS, the root port is powered off and on again. The radio's hub and
// everything behind it then get a bus reset and enumerate afresh, as after a manual reset.
#define RECOVER_AFTER_MS     10000
#define RECOVER_GAP_FAST_MS  15000    // between the first RECOVER_FAST attempts
#define RECOVER_GAP_SLOW_MS  120000   // after that
#define RECOVER_FAST         3

static int64_t s_dev_seen_ms;        // when devices were first seen without audio, 0 = not now
static int64_t s_last_recover_ms;
static int s_recover_attempts;
static volatile uint32_t s_bus_resets;

static void bus_power_cycle(uint32_t off_ms)
{
    usb_host_lib_set_root_port_power(false);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
    usb_host_lib_set_root_port_power(true);
}

static void recover_tick(void)
{
    const int64_t now = esp_timer_get_time() / 1000;
    bool streaming;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    streaming = s_streaming;
    xSemaphoreGive(s_lock);
    if (streaming) {
        s_dev_seen_ms = 0;
        s_recover_attempts = 0;
        return;
    }
    uint8_t addrs[8];
    int count = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &count) != ESP_OK || count == 0) {
        s_dev_seen_ms = 0;                 // nothing plugged in: nothing to recover
        return;
    }
    if (s_dev_seen_ms == 0) {
        s_dev_seen_ms = now;
        return;
    }
    const int64_t gap = s_recover_attempts < RECOVER_FAST ? RECOVER_GAP_FAST_MS : RECOVER_GAP_SLOW_MS;
    if (now - s_dev_seen_ms < RECOVER_AFTER_MS || (s_last_recover_ms && now - s_last_recover_ms < gap)) {
        return;
    }
    if (!tx_begin_usb_recovery()) {
        return;
    }
    s_recover_attempts++;
    s_bus_resets++;
    s_last_recover_ms = now;
    s_dev_seen_ms = 0;
    ESP_LOGW(TAG, "%d USB device(s) but no audio input for %d s: resetting the USB bus (attempt %d)", count,
             RECOVER_AFTER_MS / 1000, s_recover_attempts);
    bus_power_cycle(500);
    tx_resume();
}

void usb_audio_prepare_restart(void)
{
    ESP_LOGI(TAG, "Disconnecting the USB bus before the restart");
    usb_host_lib_set_root_port_power(false);   // the radio sees its devices unplugged, then a clean start
    vTaskDelay(pdMS_TO_TICKS(300));
}

uint32_t usb_audio_bus_resets(void)
{
    return s_bus_resets;
}

static void manager_task(void *arg)
{
    evt_t evt;
    while (1) {
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(1000)) != pdTRUE) {
            recover_tick();
            continue;
        }
        switch (evt.type) {
        case EVT_RX_CONNECTED:
            handle_rx_connected(evt.addr, evt.iface_num);
            break;
        case EVT_TX_CONNECTED:
            handle_output_connected(evt.addr, evt.iface_num);
            break;
        case EVT_DISCONNECTED:
            handle_disconnected(evt.handle);
            break;
        }
    }
}

/* Structure based on the usb_lib_task() of the espressif/usb_host_uac "audio_player" example (Apache-2.0, see NOTICE) */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .fifo_settings_custom = {
            .nptx_fifo_lines = USB_NPTX_FIFO_LINES,
            .ptx_fifo_lines = USB_PTX_FIFO_LINES,
            .rx_fifo_lines = USB_RX_FIFO_LINES,
        },
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    const unsigned fifo_depth = USB_DWC.ghwcfg3_reg.dfifodepth;
    ESP_LOGI(TAG, "USB Host installed, FIFO lines: RX %u, NPTX %u, PTX %u, hardware depth %u",
             USB_RX_FIFO_LINES, USB_NPTX_FIFO_LINES, USB_PTX_FIFO_LINES, fifo_depth);
    ESP_LOGI(TAG, "FIFO MPS limits: IN %u B, bulk/control OUT %u B, periodic OUT %u B",
             USB_RX_MPS_LIMIT, USB_BULK_OUT_MPS_LIMIT, USB_ISOC_OUT_MPS_LIMIT);
    xTaskNotifyGive((TaskHandle_t)arg);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

bool usb_audio_tx_available(void)
{
    return atomic_load(&s_tx_available);
}

bool usb_audio_tx_idle(void)
{
    return atomic_load(&s_tx_clean);
}

void usb_audio_tx_request(tx_tone_t tone, int dbfs)
{
    static unsigned generation; // calls are serialized by the TX state lock
    unsigned request = 0;
    if (tone != TX_TONE_OFF) {
        generation = (generation + 1) & 0x00ffffffu;
        request = (generation << 8) | ((unsigned)(-dbfs) << 2) | (unsigned)tone;
    }
    atomic_store(&s_tx_request, request);
    if (s_tx_task) {
        xTaskNotifyGive(s_tx_task);
    }
}

usb_tx_stats_t usb_audio_tx_stats(void)
{
    return (usb_tx_stats_t){
        .audio_rate = atomic_load(&s_tx_rate),
        .underruns = atomic_load(&s_tx_underruns),
        .written_bytes = atomic_load(&s_tx_written_bytes),
    };
}

static void tx_audio_task(void *arg)
{
    int16_t pcm[(48000 / 100) * 2];
    uac_host_device_handle_t opened = NULL;
    unsigned active = 0, bytes = 0;
    TickType_t tick = xTaskGetTickCount();
    int64_t last_write_us = 0;
    for (;;) {
        const unsigned request = atomic_load(&s_tx_request);
        if (!request && !active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const char *error = NULL;
        xSemaphoreTake(s_output_lock, portMAX_DELAY);
        if (active && (active != request || opened != s_output_handle)) {
            if (opened == s_output_handle && uac_host_device_stop(opened) != ESP_OK) {
                // No further output may start on an interface that failed to flush.
                atomic_store(&s_tx_available, false);
                error = "USB TX audio stop failed";
            } else {
                atomic_store(&s_tx_clean, true);
            }
            atomic_store(&s_tx_rate, 0);
            opened = NULL;
            active = 0;
        }

        unsigned blocks = 1;
        if (request && !active && !error && request == atomic_load(&s_tx_request)) {
            atomic_store(&s_tx_clean, false);
            atomic_store(&s_tx_transfer_error, false);
            usb_audio_tx_reset_transfer_stats();
            if (!s_output_handle || !atomic_load(&s_tx_available) ||
                uac_host_device_start(s_output_handle, &s_tx_format) != ESP_OK) {
                error = "USB TX audio start failed";
                const bool stopped = !s_output_handle || uac_host_device_stop(s_output_handle) == ESP_OK;
                atomic_store(&s_tx_clean, stopped);
                if (!stopped) {
                    atomic_store(&s_tx_available, false);
                }
            } else {
                opened = s_output_handle;
                active = request;
                tx_tone_fill(pcm, s_tx_format.sample_freq, s_tx_format.channels,
                             (tx_tone_t)(request & 3), -(int)((request >> 2) & 63));
                bytes = (s_tx_format.sample_freq / 100) * s_tx_format.channels * sizeof(pcm[0]);
                atomic_store(&s_tx_rate, s_tx_format.sample_freq);
                atomic_store(&s_tx_written_bytes, 0);
                atomic_store(&s_tx_underruns, 0);
                tick = xTaskGetTickCount();
                last_write_us = 0;
                blocks = TX_PREFILL_BLOCKS;
            }
        }
        if (active) {
            const esp_err_t submit_error = usb_audio_tx_transfer_stats().submit_error;
            if (submit_error != ESP_OK) {
                ESP_LOGW(TAG, "USB TX transfer submit: %s", esp_err_to_name(submit_error));
                error = "USB TX submit failed";
            } else if (atomic_exchange(&s_tx_transfer_error, false)) {
                error = "USB TX transfer failed";
            }
        }
        if (active && !error && active == atomic_load(&s_tx_request)) {
            const int64_t now = esp_timer_get_time();
            if (last_write_us && now - last_write_us >= 2 * TX_BLOCK_MS * 1000) {
                atomic_fetch_add(&s_tx_underruns, 1);
                tick = xTaskGetTickCount();
            }
            last_write_us = now;
            for (unsigned i = 0; i < blocks && active == atomic_load(&s_tx_request); ++i) {
                const esp_err_t err = uac_host_device_write(opened, (uint8_t *)pcm, bytes, 0);
                if (err != ESP_OK) {
                    if (err != ESP_ERR_TIMEOUT) {
                        error = "USB TX audio write failed";
                    }
                    break;
                }
                atomic_fetch_add(&s_tx_written_bytes, bytes);
            }
        }
        xSemaphoreGive(s_output_lock);
        if (error) {
            ESP_LOGW(TAG, "%s", error);
            if (atomic_load(&s_tx_request) == request) {
                tx_unkey(error);
            }
        }
        if (active) {
            xTaskDelayUntil(&tick, pdMS_TO_TICKS(TX_BLOCK_MS));
        }
    }
}

esp_err_t usb_audio_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_output_lock = xSemaphoreCreateMutex();
    s_evt_queue = xQueueCreate(8, sizeof(evt_t));
    ESP_RETURN_ON_FALSE(s_lock && s_output_lock && s_evt_queue, ESP_ERR_NO_MEM, TAG, "no memory");

    // Everything USB on core 1: usb_host_install() runs in usb_lib_task, so the USB interrupt is allocated
    // on core 1 too. On core 0 the Wi-Fi driver (priority 23) starved the USB tasks and interrupt while the
    // link was retrying, and up to 8.5 % of the radio's audio packets were lost exactly during Wi-Fi stalls.
    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(),
                                            USB_HOST_TASK_PRIORITY, NULL, USB_CORE);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "usb_lib task");
    // Wait until usb_host_install() has completed
    ESP_RETURN_ON_FALSE(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 1, ESP_ERR_TIMEOUT, TAG, "usb host install");

    const uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = USB_CORE,
        .callback = uac_driver_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(uac_host_install(&uac_config), TAG, "uac install");

    ok = xTaskCreatePinnedToCore(tx_audio_task, "usb_tx", 4096, NULL, TX_TASK_PRIORITY, &s_tx_task, USB_CORE);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "TX audio task");
    ok = xTaskCreatePinnedToCore(manager_task, "uac_mgr", 4096, NULL, MANAGER_TASK_PRIORITY, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "manager task");

    ESP_LOGI(TAG, "Waiting for a USB audio input device");
    return ESP_OK;
}

bool usb_audio_get_format(usb_audio_format_t *fmt)
{
    // The TX watchdog checks only presence. Never queue it behind a blocking PCM read.
    if (!fmt) {
        return atomic_load(&s_streaming);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool streaming = atomic_load(&s_streaming);
    if (streaming) {
        *fmt = s_fmt;
    }
    xSemaphoreGive(s_lock);
    return streaming;
}

bool usb_audio_get_output_format(usb_audio_output_format_t *fmt)
{
    xSemaphoreTake(s_output_lock, portMAX_DELAY);
    const bool present = s_output_handle != NULL;
    *fmt = s_output_format;
    xSemaphoreGive(s_output_lock);
    return present;
}

esp_err_t usb_audio_read(uint8_t *buf, uint32_t size, uint32_t *bytes_read, TickType_t timeout)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(MAX_LOCK_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_streaming) {
        ret = uac_host_device_read(s_mic_handle, buf, size, bytes_read, timeout);
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void usb_audio_flush(void)
{
    static uint8_t trash[512];
    uint32_t n = 0;
    while (usb_audio_read(trash, sizeof(trash), &n, 0) == ESP_OK && n > 0) {
    }
}
