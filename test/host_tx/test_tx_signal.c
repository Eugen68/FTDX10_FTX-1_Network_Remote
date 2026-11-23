/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "tx_swr.h"
#include "tx_tone.h"

static tx_swr_result_t sample(tx_swr_guard_t *g, unsigned raw, unsigned power, int64_t now)
{
    const cat_tx_meters_t meters = {
        .rig = CAT_RIG_FTDX10, .po = power, .swr = raw, .po_us = now, .swr_us = now,
    };
    return tx_swr_check(g, &meters, 1000000, now, 300);
}

static double energy(const int16_t *pcm, unsigned bin)
{
    double re = 0, im = 0;
    for (unsigned i = 0; i < 480; ++i) {
        const double phase = 6.283185307179586 * bin * i / 480;
        re += pcm[2*i] * cos(phase);
        im += pcm[2*i] * sin(phase);
    }
    return hypot(re, im);
}

int main(void)
{
    assert(tx_swr_value(CAT_RIG_FTDX10, 126) == 300);
    assert(tx_swr_value(CAT_RIG_FTX1, 89) == 200);
    assert(tx_swr_value(CAT_RIG_FTX1, 0) == 100);
    assert(tx_swr_value(CAT_RIG_UNKNOWN, 100) == 0);
    tx_swr_guard_t guard = {0};
    assert(sample(&guard, 126, 2, 1000000) == TX_SWR_OK);
    assert(sample(&guard, 126, 3, 1200000) == TX_SWR_LIMIT);
    guard = (tx_swr_guard_t){0};
    assert(sample(&guard, 50, 10, 1000000) == TX_SWR_OK);
    assert(sample(&guard, 50, 10, 1900000) == TX_SWR_OK);
    for (int64_t now = 2000000; now <= 2200000; now += 100000) {
        assert(sample(&guard, 50, 10, now) == TX_SWR_OK);
    }
    assert(guard.reference_count == 3);
    assert(sample(&guard, 89, 10, 2300000) == TX_SWR_OK);
    assert(sample(&guard, 89, 10, 2300000) == TX_SWR_OK);
    assert(sample(&guard, 89, 10, 2400000) == TX_SWR_OK);
    assert(sample(&guard, 89, 10, 2500000) == TX_SWR_RISE);

    guard = (tx_swr_guard_t){0};
    cat_tx_meters_t missing = {.rig = CAT_RIG_FTDX10};
    assert(tx_swr_check(&guard, &missing, 1000000, 1500000, 300) == TX_SWR_OK);
    assert(tx_swr_check(&guard, &missing, 1000000, 1700000, 300) == TX_SWR_STALE);
    missing.po_us = 1700000;
    missing.po = 20;
    assert(tx_swr_check(&guard, &missing, 1000000, 1700000, 300) == TX_SWR_OK);
    missing.po_us = 2400000;
    assert(tx_swr_check(&guard, &missing, 1000000, 2400000, 300) == TX_SWR_STALE);
    missing.po = 0;
    assert(tx_swr_check(&guard, &missing, 1000000, 2400000, 300) == TX_SWR_OK);

    int16_t pcm[960];
    tx_tone_fill(pcm, 48000, 2, TX_TONE_SINGLE, -24);
    assert(energy(pcm, 10) > 400000 && energy(pcm, 7) < 100);
    for (unsigned i = 0; i < 960; i += 2) {
        assert(pcm[i] == pcm[i+1] && abs(pcm[i]) <= 2068);
    }
    tx_tone_fill(pcm, 48000, 2, TX_TONE_DUAL, -24);
    assert(energy(pcm, 7) > 200000 && energy(pcm, 19) > 200000 && energy(pcm, 10) < 100);
    puts("SWR guard and USB tone tests passed");
}
