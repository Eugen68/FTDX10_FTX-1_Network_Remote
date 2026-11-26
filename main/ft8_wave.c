/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "ft8_wave.h"

#include <math.h>
#include <stddef.h>
#include "ft8/encode.h"
#include "ft8/message.h"

#define PI 3.14159265358979323846f
#define RAMP (FT8_TX_SYMBOL_SAMPLES / 8)
#define PULSE_STEPS 256

bool ft8_wave_pack(const char *message, uint8_t tones[FT8_TX_SYMBOLS])
{
    char normalized[FT8_TX_TEXT_SIZE] = {0};
    size_t n = 0;
    bool space = false;
    for (; *message; ++message) {
        char c = *message;
        if (c == ' ') {
            space = n != 0;
            continue;
        }
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c < 0x21 || c > 0x7e || n + (space ? 1 : 0) >= sizeof(normalized) - 1) {
            return false;
        }
        if (space) normalized[n++] = ' ';
        normalized[n++] = c;
        space = false;
    }
    ftx_message_t packed;
    ftx_message_init(&packed);
    if (!n || ftx_message_encode(&packed, NULL, normalized) != FTX_MESSAGE_RC_OK) {
        return false;
    }
    ft8_encode(packed.payload, tones);
    return true;
}

// BT=2 Gaussian-shaped frequency pulse centered on a 160 ms symbol.
static float pulse(float x)
{
    const float k = 5.336446f * 2;
    return (erff(k * (x + 0.5f)) - erff(k * (x - 0.5f))) * 0.5f;
}

bool ft8_wave_render(int16_t *pcm, const uint8_t tones[FT8_TX_SYMBOLS], unsigned hz, int dbfs,
                     bool (*yield)(void))
{
    const float level = 32767.f * powf(10.f, dbfs / 20.f);
    const float carrier = 2.f * PI * hz / FT8_TX_RATE;
    const float shift = 2.f * PI / FT8_TX_SYMBOL_SAMPLES;
    float gaussian[PULSE_STEPS + 1], edge[PULSE_STEPS + 1];
    for (unsigned k = 0; k <= PULSE_STEPS; ++k) {
        float fraction = (float)k / PULSE_STEPS;
        gaussian[k] = pulse(fraction - 0.5f);
        edge[k] = pulse(fraction + 0.5f);
    }

    float phase = 0;
    for (unsigned symbol = 0; symbol < FT8_TX_SYMBOLS; ++symbol) {
        unsigned previous = tones[symbol ? symbol - 1 : 0];
        unsigned next = tones[symbol + 1 < FT8_TX_SYMBOLS ? symbol + 1 : symbol];
        for (unsigned i = 0; i < FT8_TX_SYMBOL_SAMPLES; ++i) {
            unsigned k = (i * PULSE_STEPS + FT8_TX_SYMBOL_SAMPLES / 2) / FT8_TX_SYMBOL_SAMPLES;
            unsigned sample = symbol * FT8_TX_SYMBOL_SAMPLES + i;
            unsigned tail = FT8_TX_SAMPLES - 1 - sample;
            unsigned edge_sample = sample < tail ? sample : tail;
            float gain = edge_sample < RAMP ? (1.f - cosf(PI * edge_sample / RAMP)) * 0.5f : 1.f;
            pcm[sample] = (int16_t)(sinf(phase) * level * gain);
            phase += carrier + shift * (previous * edge[k] + tones[symbol] * gaussian[k] +
                                        next * edge[PULSE_STEPS - k]);
            if (phase >= 2.f * PI) phase -= 2.f * PI;
        }
        if (yield && !yield()) return false;
    }
    return true;
}

int64_t ft8_next_slot(int64_t utc_us, bool odd, int64_t lead_us)
{
    const int64_t earliest = utc_us + lead_us;
    int64_t slot = (earliest + FT8_TX_SLOT_US - 1) / FT8_TX_SLOT_US;
    if ((slot & 1) != odd) ++slot;
    return slot * FT8_TX_SLOT_US;
}
