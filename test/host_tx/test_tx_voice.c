/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdio.h>
#include "tx_voice.h"

int main(void)
{
    uint8_t frame[2 * TX_VOICE_SAMPLES];
    int16_t out[960];
    for (unsigned i = 0; i < sizeof(frame); i += 2) {
        frame[i] = 0x00;
        frame[i + 1] = 0x10;
    }
    assert(tx_voice_start());
    for (unsigned i = 0; i < 3; ++i) tx_voice_push(frame);
    tx_voice_render(out, 48000, 2);
    for (unsigned i = 0; i < 960; ++i) assert(out[i] == 0);
    assert(tx_voice_stats().level_ms == 60 && tx_voice_stats().underruns == 0);
    tx_voice_push(frame);
    tx_voice_render(out, 48000, 2);
    for (unsigned i = 0; i < 960; i += 2) {
        assert(out[i] >= 4095 && out[i] <= 4097 && out[i] == out[i + 1]);
    }
    for (unsigned i = 0; i < 20; ++i) tx_voice_render(out, 48000, 1);
    assert(tx_voice_stats().underruns == 1 && tx_voice_stats().target_ms == 100);
    for (unsigned i = 0; i < 480; ++i) assert(out[i] == 0);
    for (unsigned i = 0; i < 45; ++i) tx_voice_push(frame);
    assert(tx_voice_stats().level_ms <= 600 && tx_voice_stats().dropped_frames > 0);
    tx_voice_render(out, 48000, 1);
    assert(out[0] != 0 && out[479] != 0 && tx_voice_stats().rate_ppm == 5000);
    tx_voice_stop();
    assert(tx_voice_stats().level_ms == 0 && tx_voice_stats().rate_ppm == 0);
    assert(tx_voice_start());
    assert(tx_voice_stats().target_ms == 80 && tx_voice_stats().underruns == 0);
    puts("Voice prefill, resampling, underrun, backlog and reset passed");
}
