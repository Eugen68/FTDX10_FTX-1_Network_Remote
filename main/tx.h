/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { TX_IDLE, TX_ARMED, TX_KEYED } tx_phase_t;

typedef struct {
    tx_phase_t phase;
    uint32_t owner;
    unsigned left_s;
    const char *reason;
    bool on_air;
} tx_state_t;

esp_err_t tx_init(void);
bool tx_acquire(uint32_t owner);
bool tx_key(uint32_t owner);
void tx_heartbeat(uint32_t owner);
void tx_release(uint32_t owner, const char *reason);
void tx_unkey(const char *reason);
tx_state_t tx_get_state(void);
bool tx_begin_usb_recovery(void);
bool tx_quiesce(const char *reason);
void tx_resume(void);
