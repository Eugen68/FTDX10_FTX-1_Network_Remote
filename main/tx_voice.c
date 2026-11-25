/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tx_voice.h"

enum {
    MIN_PREFILL = 4 * TX_VOICE_SAMPLES,
    MAX_PREFILL = 10 * TX_VOICE_SAMPLES,
    CAPACITY = 30 * TX_VOICE_SAMPLES,
    TAPS = 16,
    PHASES = 32,
    ONE = 65536,
};

static _Atomic(SemaphoreHandle_t) s_lock;
static int16_t *s_ring;
static unsigned s_read, s_used, s_target = MIN_PREFILL, s_phase;
static bool s_playing;
static float s_fir[PHASES][TAPS];
static tx_voice_stats_t s_stats;

bool tx_voice_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ring) {
        s_ring = heap_caps_malloc(CAPACITY * sizeof(*s_ring), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            xSemaphoreGive(s_lock);
            return false;
        }
        const float pi = 3.141592653589793f;
        for (unsigned p = 0; p < PHASES; ++p) {
            float sum = 0;
            for (unsigned k = 0; k < TAPS; ++k) {
                const float t = 0.85f * pi * (k - (TAPS / 2 - 1 + (float)p / PHASES));
                const float window = 0.5f - 0.5f * cosf(2 * pi * k / (TAPS - 1));
                s_fir[p][k] = window * (t == 0 ? 1 : sinf(t) / t);
                sum += s_fir[p][k];
            }
            for (unsigned k = 0; k < TAPS; ++k) s_fir[p][k] /= sum;
        }
    }
    s_read = s_used = s_phase = 0;
    s_target = MIN_PREFILL;
    s_playing = false;
    s_stats = (tx_voice_stats_t){0};
    xSemaphoreGive(s_lock);
    return true;
}

void tx_voice_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_read = s_used = s_phase = 0;
    s_playing = false;
    s_stats.rate_ppm = 0;
    xSemaphoreGive(s_lock);
}

void tx_voice_push(const uint8_t pcm[2 * TX_VOICE_SAMPLES])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_used + TX_VOICE_SAMPLES > CAPACITY) {
        const unsigned discard = s_used - s_target;
        s_read = (s_read + discard) % CAPACITY;
        s_used -= discard;
        s_phase = 0;
        s_stats.dropped_frames += (discard + TX_VOICE_SAMPLES - 1) / TX_VOICE_SAMPLES;
    }
    unsigned pos = (s_read + s_used) % CAPACITY;
    for (unsigned i = 0; i < TX_VOICE_SAMPLES; ++i) {
        s_ring[pos] = (int16_t)(pcm[2 * i] | (uint16_t)pcm[2 * i + 1] << 8);
        if (++pos == CAPACITY) pos = 0;
    }
    s_used += TX_VOICE_SAMPLES;
    xSemaphoreGive(s_lock);
}

void tx_voice_render(int16_t *pcm, unsigned rate, unsigned channels)
{
    const unsigned samples = rate / 100;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_playing) s_playing = s_used >= s_target;
    const int error = (int)s_used - (int)s_target;
    int ppm = error * 8;
    if (ppm > 5000) ppm = 5000;
    if (ppm < -5000) ppm = -5000;
    s_stats.rate_ppm = s_playing ? ppm : 0;
    const unsigned nominal_step = ((unsigned)TX_VOICE_RATE * ONE + rate / 2) / rate;
    const unsigned step = nominal_step + (int)nominal_step * s_stats.rate_ppm / 1000000;

    unsigned i = 0;
    if (s_playing) {
        for (; i < samples && s_used >= TAPS; ++i) {
            const float *kernel = s_fir[s_phase * PHASES / ONE];
            unsigned pos = s_read;
            float value = 0;
            for (unsigned k = 0; k < TAPS; ++k) {
                value += kernel[k] * s_ring[pos];
                if (++pos == CAPACITY) pos = 0;
            }
            const int16_t sample = (int16_t)fmaxf(-32768, fminf(32767, value));
            for (unsigned c = 0; c < channels; ++c) *pcm++ = sample;
            s_phase += step;
            if (s_phase >= ONE) {
                s_phase -= ONE;
                if (++s_read == CAPACITY) s_read = 0;
                --s_used;
            }
        }
        if (i < samples) {
            s_playing = false;
            s_read = s_used = s_phase = 0;
            s_stats.rate_ppm = 0;
            ++s_stats.underruns;
            if (s_target < MAX_PREFILL) s_target += TX_VOICE_SAMPLES;
        }
    }
    memset(pcm, 0, (samples - i) * channels * sizeof(*pcm));
    xSemaphoreGive(s_lock);
}

tx_voice_stats_t tx_voice_stats(void)
{
    if (!s_lock) return (tx_voice_stats_t){.target_ms = 80};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_voice_stats_t stats = s_stats;
    stats.level_ms = s_used / (TX_VOICE_RATE / 1000);
    stats.target_ms = s_target / (TX_VOICE_RATE / 1000);
    xSemaphoreGive(s_lock);
    return stats;
}
