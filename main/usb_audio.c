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
#include "usb_audio.h"

// Above the stream's USB reader (6), the web server and LwIP (18), below the Wi-Fi driver (23): the
// isochronous transfers must be handed back to the controller on time or audio packets are lost.
// Both tasks only move small buffers, so they cost little CPU.
#define USB_HOST_TASK_PRIORITY  20
#define UAC_TASK_PRIORITY       20
#define MANAGER_TASK_PRIORITY   4
#define PREFERRED_BITS          16
#define MAX_LOCK_WAIT_MS        500

typedef enum {
    EVT_RX_CONNECTED,
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
static SemaphoreHandle_t s_lock;               // protects s_mic_handle / s_fmt / s_streaming
static uac_host_device_handle_t s_mic_handle;
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
    }
    // TX (speaker) devices are not used
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

static void handle_disconnected(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (handle == s_mic_handle) {
        s_streaming = false;
        s_mic_handle = NULL;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Audio input disconnected");
    uac_host_device_close(handle);
}

static void manager_task(void *arg)
{
    evt_t evt;
    while (1) {
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case EVT_RX_CONNECTED:
            handle_rx_connected(evt.addr, evt.iface_num);
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

    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(),
                                            USB_HOST_TASK_PRIORITY, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "usb_lib task");
    // Wait until usb_host_install() has completed
    ESP_RETURN_ON_FALSE(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 1, ESP_ERR_TIMEOUT, TAG, "usb host install");

    const uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
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
