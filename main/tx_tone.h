/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum { TX_TONE_OFF, TX_TONE_SINGLE, TX_TONE_DUAL, TX_TONE_VOICE, TX_TONE_FT8 } tx_tone_t;
#define TX_TONE_DEFAULT_DBFS (-24)
#define TX_TONE_MIN_DBFS (-60)
#define TX_TONE_MAX_DBFS (-6)

/** Generate a 10 ms repeating PCM period. Dual-tone level is the combined peak bound. */
void tx_tone_fill(int16_t *buffer, unsigned rate, unsigned channels, tx_tone_t tone, int dbfs);
