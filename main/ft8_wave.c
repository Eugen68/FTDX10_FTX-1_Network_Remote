/* SPDX-License-Identifier: MIT */
#include "ft8_wave.h"

#include <math.h>
#include <stddef.h>
#include "ft8/encode.h"
#include "ft8/message.h"

#define PI 3.14159265358979323846f
#define RAMP (FT8_TX_SYMBOL_SAMPLES / 8)
#define PULSE_STEPS 256
#define SINE_BITS 10
#define SINE_SIZE (1u << SINE_BITS)
#define INTERP_BITS 8
#define PHASE_PER_TURN 4294967296.0

_Static_assert(FT8_TX_SYMBOL_SAMPLES % PULSE_STEPS == 0, "whole sample pulse steps");

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

// Owned by the single FT8 renderer and reused after the first frame.
static int16_t sine[SINE_SIZE + 1];
static int16_t ramp[RAMP];
static uint32_t gaussian[PULSE_STEPS + 1], edge[PULSE_STEPS + 1];

static void prepare_tables(void)
{
    static bool ready;
    if (ready) return;
    for (unsigned i = 0; i < SINE_SIZE; ++i) {
        sine[i] = (int16_t)lroundf(sinf(2.f * PI * i / SINE_SIZE) * 32767.f);
    }
    sine[SINE_SIZE] = sine[0];
    for (unsigned i = 0; i < RAMP; ++i) {
        ramp[i] = (int16_t)lroundf((1.f - cosf(PI * i / RAMP)) * 16383.5f);
    }
    const float phase_step = (float)(PHASE_PER_TURN / FT8_TX_SYMBOL_SAMPLES);
    for (unsigned k = 0; k <= PULSE_STEPS; ++k) {
        const float fraction = (float)k / PULSE_STEPS;
        gaussian[k] = (uint32_t)lroundf(pulse(fraction - 0.5f) * phase_step);
        edge[k] = (uint32_t)lroundf(pulse(fraction + 0.5f) * phase_step);
    }
    ready = true;
}

bool ft8_wave_render(int16_t *pcm, const uint8_t tones[FT8_TX_SYMBOLS], unsigned hz, int dbfs,
                     bool (*yield)(void))
{
    prepare_tables();
    const int32_t level = (int32_t)lroundf(32767.f * powf(10.f, dbfs / 20.f));
    const uint32_t carrier = (uint32_t)(hz * (PHASE_PER_TURN / FT8_TX_RATE) + 0.5);
    const unsigned step_samples = FT8_TX_SYMBOL_SAMPLES / PULSE_STEPS;
    unsigned sample = 0;
    uint32_t phase = 0; // unsigned wrap is one turn
    for (unsigned symbol = 0; symbol < FT8_TX_SYMBOLS; ++symbol) {
        const unsigned previous = tones[symbol ? symbol - 1 : 0];
        const unsigned next = tones[symbol + 1 < FT8_TX_SYMBOLS ? symbol + 1 : symbol];
        for (unsigned k = 0; k <= PULSE_STEPS; ++k) {
            const uint32_t delta = carrier + previous * edge[k] + tones[symbol] * gaussian[k] +
                                   next * edge[PULSE_STEPS - k];
            // Match the original nearest-pulse sampling at the half steps.
            const unsigned count = k == 0 || k == PULSE_STEPS ? step_samples / 2 : step_samples;
            for (unsigned i = 0; i < count; ++i, ++sample) {
                const unsigned index = phase >> (32 - SINE_BITS);
                const int32_t fraction = (phase >> (32 - SINE_BITS - INTERP_BITS)) & ((1u << INTERP_BITS) - 1);
                const int32_t value = sine[index] +
                    (((int32_t)sine[index + 1] - sine[index]) * fraction >> INTERP_BITS);
                int32_t out = (value * level) / 32768;
                const unsigned tail = FT8_TX_SAMPLES - 1 - sample;
                const unsigned edge_sample = sample < tail ? sample : tail;
                if (edge_sample < RAMP) out = (out * ramp[edge_sample]) / 32768;
                pcm[sample] = (int16_t)out;
                phase += delta;
            }
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
