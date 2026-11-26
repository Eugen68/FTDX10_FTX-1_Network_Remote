/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once
#include "esp_err.h"
#include "ft8_wave.h"

typedef struct {
    bool armed;
    bool owned;
    bool preparing;
    const char *reason;
    const char *clock;
    int64_t age_ms;
    int64_t offset_ms;
    unsigned rtt_ms, lead_ms, prepare_ms;
    unsigned remaining;
} ft8_tx_status_t;

#define FT8_UTC_TEXT_SIZE 25

void ft8_format_utc(int64_t utc_us, char timestamp[FT8_UTC_TEXT_SIZE]);
esp_err_t ft8_tx_init(void);
void ft8_tx_start_clock(void);
const char *ft8_tx_arm(uint32_t owner, const char *message, unsigned hz, bool odd, int dbfs);
void ft8_tx_halt(uint32_t owner, const char *reason);
void ft8_tx_heartbeat(uint32_t owner);
void ft8_tx_browser_clock(uint32_t owner, int64_t mono_to_utc_ms, unsigned rtt_ms);
ft8_tx_status_t ft8_tx_status(uint32_t owner);
void ft8_tx_copy(int16_t *pcm, unsigned offset, unsigned count, unsigned channels);
