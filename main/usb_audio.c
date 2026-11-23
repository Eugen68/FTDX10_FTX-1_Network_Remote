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
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "esp_timer.h"
#include "cat.h"
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
#define OUTPUT_PROBE_BUFFER     256      // uac_host_device_open() requires a nonempty ring buffer

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
static uac_host_device_handle_t s_output_handle;
static uac_host_dev_alt_param_t *s_output_formats;
static usb_audio_format_t s_fmt;
static bool s_streaming;

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

static void handle_output_connected(uint8_t addr, uint8_t iface_num)
{
    if (s_output_handle != NULL) {
        ESP_LOGW(TAG, "An audio output is already open, ignoring the new one");
        return;
    }

    const uac_host_device_config_t config = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = OUTPUT_PROBE_BUFFER,
        .callback = uac_device_cb,
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
    uac_host_dev_info_t info;
    uac_host_dev_alt_param_t *formats = NULL;
    if (uac_host_get_device_info(handle, &info) == ESP_OK && info.iface_alt_num > 0) {
        formats = calloc(info.iface_alt_num, sizeof(*formats));
        if (formats != NULL) {
            for (unsigned i = 0; i < info.iface_alt_num; i++) {
                if (uac_host_get_device_alt_param(handle, i + 1, &formats[i]) != ESP_OK) {
                    free(formats);
                    formats = NULL;
                    break;
                }
            }
        }
    }
    if (formats == NULL) {
        ESP_LOGW(TAG, "Unable to cache audio output formats");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_output_handle = handle;
    s_output_formats = formats;
    xSemaphoreGive(s_lock);
}

static void handle_disconnected(uac_host_device_handle_t handle)
{
    uac_host_dev_alt_param_t *formats = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool output = handle == s_output_handle;
    if (output) {
        s_output_handle = NULL;
        formats = s_output_formats;
        s_output_formats = NULL;
    } else if (handle == s_mic_handle) {
        s_streaming = false;
        s_mic_handle = NULL;
    }
    xSemaphoreGive(s_lock);

    free(formats);
    ESP_LOGI(TAG, "Audio %s disconnected", output ? "output" : "input");
    uac_host_device_close(handle);
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
    if (!cat_radio_rx_confirmed()) {
        return;
    }
    s_recover_attempts++;
    s_bus_resets++;
    s_last_recover_ms = now;
    s_dev_seen_ms = 0;
    ESP_LOGW(TAG, "%d USB device(s) but no audio input for %d s: resetting the USB bus (attempt %d)", count,
             RECOVER_AFTER_MS / 1000, s_recover_attempts);
    bus_power_cycle(500);
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
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive((TaskHandle_t)arg);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

esp_err_t usb_audio_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_evt_queue = xQueueCreate(8, sizeof(evt_t));
    ESP_RETURN_ON_FALSE(s_lock && s_evt_queue, ESP_ERR_NO_MEM, TAG, "no memory");

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

    ok = xTaskCreatePinnedToCore(manager_task, "uac_mgr", 4096, NULL, MANAGER_TASK_PRIORITY, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "manager task");

    ESP_LOGI(TAG, "Waiting for a USB audio input device");
    return ESP_OK;
}

bool usb_audio_get_format(usb_audio_format_t *fmt)
{
    bool streaming;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    streaming = s_streaming;
    if (streaming && fmt) {
        *fmt = s_fmt;
    }
    xSemaphoreGive(s_lock);
    return streaming;
}

bool usb_audio_get_output_format(usb_audio_output_format_t *fmt)
{
    *fmt = (usb_audio_output_format_t){0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool present = s_output_handle != NULL;
    if (s_output_formats != NULL) {
        const uac_host_dev_alt_param_t *alt = &s_output_formats[0];
        fmt->channels = alt->channels;
        fmt->bits = alt->bit_resolution;
        if (alt->sample_freq_type == 0) {
            fmt->rate_min = alt->sample_freq_lower;
            fmt->rate_max = alt->sample_freq_upper;
        } else {
            fmt->rate_count = alt->sample_freq_type < UAC_FREQ_NUM_MAX
                            ? alt->sample_freq_type : UAC_FREQ_NUM_MAX;
            memcpy(fmt->rates, alt->sample_freq, fmt->rate_count * sizeof(fmt->rates[0]));
        }
    }
    xSemaphoreGive(s_lock);
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
