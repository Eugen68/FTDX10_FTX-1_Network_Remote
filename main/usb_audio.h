/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Receiving the radio's USB audio (see usb_audio.c): stream format and PCM read functions.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PCM format of the currently streaming USB audio input
 */
typedef struct {
    uint32_t sample_rate;   /*!< Sampling frequency in Hz */
    uint8_t channels;       /*!< Number of channels */
    uint8_t subframe_size;  /*!< Bytes per sample (container size) */
    uint8_t bit_resolution; /*!< Significant bits per sample */
} usb_audio_format_t;

/**
 * @brief Install USB Host Library and UAC class driver
 *
 * When a USB audio input (microphone / line-in) device is attached, it is opened
 * and its stream is started automatically. When it is detached, the stream is closed.
 */
esp_err_t usb_audio_init(void);

/**
 * @brief Get the format of the running stream
 *
 * @return true if a USB audio input device is currently streaming
 */
bool usb_audio_get_format(usb_audio_format_t *fmt);

/**
 * @brief Read PCM data captured from the USB audio device
 *
 * @param[out] buf        Destination buffer
 * @param[in]  size       Bytes requested
 * @param[out] bytes_read Bytes actually read
 * @param[in]  timeout    Timeout in ticks
 *
 * @return ESP_OK, ESP_ERR_TIMEOUT if no data arrived, ESP_ERR_INVALID_STATE if no device is streaming
 */
esp_err_t usb_audio_read(uint8_t *buf, uint32_t size, uint32_t *bytes_read, TickType_t timeout);

/**
 * @brief Discard data currently buffered by the driver
 */
void usb_audio_flush(void);

/**
 * @brief Power the USB root port off before a software restart, so the radio sees its devices unplugged
 *        and enumerates afresh after the restart (a restart alone left the radio's audio stuck at times)
 */
void usb_audio_prepare_restart(void);

/** @brief How often the USB bus was reset because the radio's audio did not come up */
uint32_t usb_audio_bus_resets(void);

#ifdef __cplusplus
}
#endif
