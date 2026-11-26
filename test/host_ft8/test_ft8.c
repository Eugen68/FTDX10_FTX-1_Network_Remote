/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft8_wave.h"

static unsigned rendered_symbols;

static bool pause_render(void)
{
    return ++rendered_symbols < 2;
}

int main(void)
{
    uint8_t tones[FT8_TX_SYMBOLS], normalized[FT8_TX_SYMBOLS];
    assert(ft8_wave_pack("CQ K1ABC FN42", tones));
    assert(ft8_wave_pack(" cq  k1abc fn42 ", normalized));
    assert(memcmp(tones, normalized, sizeof(tones)) == 0);
    // Derived independently from the Type 1 packing, CRC-14, LDPC generator,
    // and Gray mapping in WSJT-X commit 967c85a6119110788b0df3782cf796485b2cee14.
    // Message: CQ K1ABC FN42. Compare all sync and data symbols.
    const char reference[] =
        "3140652000000001005476704606021533433140652736011047517007334745455133543140652";
    _Static_assert(sizeof(reference) - 1 == FT8_TX_SYMBOLS, "reference contains 79 tones");
    for (unsigned i = 0; i < FT8_TX_SYMBOLS; ++i) assert(tones[i] == reference[i] - '0');
    assert(!ft8_wave_pack(" ", normalized));
    assert(!ft8_wave_pack("THIS MESSAGE IS FAR TOO LONG TO ENCODE", normalized));
    assert(!ft8_wave_pack("CQ\nK1ABC", normalized));

    int16_t *audio = malloc(FT8_TX_SAMPLES * sizeof(*audio));
    assert(audio && !ft8_wave_render(audio, tones, 1500, -24, pause_render));
    assert(rendered_symbols == 2);
    assert(ft8_wave_render(audio, tones, 1500, -24, NULL));
    assert(audio[0] == 0 && audio[FT8_TX_SAMPLES - 1] == 0);
    int peak = 0;
    for (unsigned i = 0; i < FT8_TX_SAMPLES; ++i) {
        if (abs(audio[i]) > peak) peak = abs(audio[i]);
    }
    assert(peak >= 2060 && peak <= 2070);
    // The first Costas tones are 3 and 1: detect the 12.5 Hz frequency change.
    const unsigned offset[] = {2000, FT8_TX_SYMBOL_SAMPLES + 2000};
    unsigned crossings[2] = {0};
    for (unsigned j = 0; j < 2; ++j) {
        for (unsigned i = offset[j] + 1; i < offset[j] + 4800; ++i) {
            if (audio[i] >= 0 && audio[i - 1] < 0) ++crossings[j];
        }
    }
    assert(crossings[0] > crossings[1]);
    free(audio);

    assert(ft8_next_slot(28500000, false, 1000000) == 30000000);
    assert(ft8_next_slot(29000001, false, 1000000) == 60000000);
    assert(ft8_next_slot(30000001, true, 1000000) == 45000000);
    assert(ft8_next_slot(14900000, true, 300000) == 45000000);
    puts("FT8 message packing, Costas blocks, waveform, and slot parity passed");
}
