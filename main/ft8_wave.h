/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FT8_TX_SYMBOLS 79
#define FT8_TX_RATE 48000
#define FT8_TX_SYMBOL_SAMPLES (FT8_TX_RATE * 160 / 1000)
#define FT8_TX_SAMPLES (FT8_TX_SYMBOLS * FT8_TX_SYMBOL_SAMPLES)
#define FT8_TX_FRAME_US 12640000LL
#define FT8_TX_SLOT_US 15000000LL
#define FT8_TX_AUDIO_US 500000LL
#define FT8_TX_TEXT_SIZE 35

bool ft8_wave_pack(const char *message, uint8_t tones[FT8_TX_SYMBOLS]);
bool ft8_wave_render(int16_t *pcm, const uint8_t tones[FT8_TX_SYMBOLS], unsigned hz, int dbfs,
                     bool (*yield)(void));
int64_t ft8_next_slot(int64_t utc_us, bool odd, int64_t lead_us);
