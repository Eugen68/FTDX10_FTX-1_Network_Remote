/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "tx_tone.h"

typedef enum { TX_IDLE, TX_ARMED, TX_KEYED } tx_phase_t;

typedef struct {
    tx_phase_t phase;
    uint32_t owner;
    unsigned left_s;
    const char *reason;
    bool on_air;
    bool stopping;
    bool rx_confirmed;
    unsigned off_attempts;
    esp_err_t off_error;
    unsigned swr, swr_raw, po_raw, stop_swr;
    bool swr_valid, po_valid;
} tx_state_t;

esp_err_t tx_init(void);
bool tx_acquire(uint32_t owner);
bool tx_key(uint32_t owner);
bool tx_key_tone(uint32_t owner, tx_tone_t tone, int dbfs);
bool tx_submit_voice(uint32_t owner, const uint8_t *frame, size_t length);
void tx_heartbeat(uint32_t owner);
void tx_release(uint32_t owner, const char *reason);
void tx_unkey(const char *reason);
tx_state_t tx_get_state(void);
bool tx_begin_usb_recovery(void);
bool tx_quiesce(const char *reason);
void tx_resume(void);
