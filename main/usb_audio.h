/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Receiving the radio's USB audio (see usb_audio.c): stream format and PCM read functions.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "tx_tone.h"

#define USB_AUDIO_TX_RING_BYTES (16 * 1024)
#define USB_FT8_LATE_GAP_US 20000U

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

/** Selected output alternate setting. A continuous range has rate_count == 0. */
typedef struct {
    uint32_t rates[CONFIG_UAC_FREQ_NUM_MAX];
    uint32_t rate_min;
    uint32_t rate_max;
    uint8_t rate_count;
    uint8_t channels;
    uint8_t bits;
    uint8_t endpoints, attributes;
    uint16_t mps;  /*!< Endpoint maximum packet size in bytes */
} usb_audio_output_format_t;

/** Copy the output descriptor summary. Returns true while the interface is open. */
bool usb_audio_get_output_format(usb_audio_output_format_t *fmt);

typedef struct {
    unsigned audio_rate, underruns, written_bytes;
} usb_tx_stats_t;

typedef struct {
    unsigned completed_bytes, completed_packets, failed_packets, pcm_peak;
    esp_err_t submit_error;
} usb_tx_transfer_stats_t;

typedef struct {
    unsigned count;
    int first_ms, last_ms;
} ft8_audio_event_t;

typedef struct {
    ft8_audio_event_t task_late, write_timeout, write_failed, iso_failed, iso_short, queue_empty;
    unsigned max_task_gap_us;
} ft8_audio_stats_t;

ft8_audio_stats_t usb_audio_ft8_stats(void);
void usb_audio_ft8_iso_event(unsigned failed, unsigned incomplete, bool drained, unsigned attempted_bytes);
void usb_audio_tx_reset_transfer_stats(void);
usb_tx_transfer_stats_t usb_audio_tx_transfer_stats(void);
bool usb_audio_tx_available(void);
bool usb_audio_tx_idle(void);
esp_err_t usb_audio_tx_prepare(bool share_rx_channel);
bool usb_audio_tx_cleanup_pending(void);
void usb_audio_tx_finished(void);
bool usb_audio_rx_paused(void);
bool usb_audio_ft8_complete(void);
void usb_audio_tx_request(tx_tone_t tone, int dbfs);
usb_tx_stats_t usb_audio_tx_stats(void);

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
 *        ESP_ERR_NOT_FINISHED indicates an intentional RX pause for USB TX.
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

/** @brief One USB audio event kept for /status: what, when, and the internal RAM at that moment */
typedef struct {
    const char *reason;     /*!< NULL if it never happened since boot */
    int64_t at_us;
    uint32_t int_free, int_min;
} usb_audio_event_t;

/** @brief The last loss of the radio's USB audio input and the last USB bus reset */
void usb_audio_last_events(usb_audio_event_t *loss, usb_audio_event_t *reset);
bool usb_audio_recover_cat(unsigned attempt);

#ifdef __cplusplus
}
#endif
