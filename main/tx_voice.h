/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { TX_VOICE_RATE = 16000, TX_VOICE_SAMPLES = 320, TX_VOICE_FRAME_BYTES = 2 + 2 * TX_VOICE_SAMPLES };

typedef struct {
    unsigned level_ms, target_ms, underruns, dropped_frames;
    int rate_ppm;
} tx_voice_stats_t;

bool tx_voice_start(void);
void tx_voice_stop(void);
void tx_voice_push(const uint8_t pcm[2 * TX_VOICE_SAMPLES]);
void tx_voice_render(int16_t *pcm, unsigned rate, unsigned channels);
tx_voice_stats_t tx_voice_stats(void);
