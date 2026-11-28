/* SPDX-License-Identifier: MIT */
#include "tx_tone.h"

#include <math.h>

void tx_tone_fill(int16_t *buffer, unsigned rate, unsigned channels, tx_tone_t tone, int dbfs)
{
    const unsigned frames = rate / 100;
    const float peak = 32767.0f * powf(10.0f, dbfs / 20.0f);
    const float step = 6.283185307179586f / frames;
    for (unsigned i = 0; i < frames; ++i) {
        const float phase = step * i;
        float sample = 0;
        if (tone == TX_TONE_SINGLE) {
            sample = sinf(phase * 10);
        } else if (tone == TX_TONE_DUAL) {
            sample = 0.5f * (sinf(phase * 7) + sinf(phase * 19));
        }
        const int16_t pcm = (int16_t)lrintf(peak * sample);
        for (unsigned ch = 0; ch < channels; ++ch) {
            buffer[i * channels + ch] = pcm;
        }
    }
}
