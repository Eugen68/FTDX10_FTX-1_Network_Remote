/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "tx_swr.h"
#include "tx_tone.h"

static tx_swr_result_t sample(tx_swr_guard_t *g, unsigned raw, unsigned power, int64_t now)
{
    const cat_tx_meters_t meters = {
        .rig = CAT_RIG_FTDX10, .po = power, .swr = raw, .swr_po = power, .po_us = now,
        .swr_us = now, .swr_po_us = now,
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
    assert(tx_swr_check(&guard, &missing, 1000000, 1700000, 300) == TX_SWR_OK);
    assert(tx_swr_check(&guard, &missing, 1000000, 2100000, 300) == TX_SWR_OK);
    assert(tx_swr_check(&guard, &missing, 1000000, 2100001, 300) == TX_SWR_STALE);
    missing.po_us = 1700000;
    missing.po = 20;
    assert(tx_swr_check(&guard, &missing, 1000000, 1700000, 300) == TX_SWR_OK);
    missing.po_us = 2400000;
    assert(tx_swr_check(&guard, &missing, 1000000, 2400000, 300) == TX_SWR_OK);
    missing.po_us = 2800001;
    assert(tx_swr_check(&guard, &missing, 1000000, 2800001, 300) == TX_SWR_STALE);
    missing.po = 0;
    assert(tx_swr_check(&guard, &missing, 1000000, 2800001, 300) == TX_SWR_OK);

    // Delay a previously fresh pair for 0.7 s. A valid high SWR still trips at once.
    guard = (tx_swr_guard_t){0};
    assert(sample(&guard, 50, 10, 1000000) == TX_SWR_OK);
    cat_tx_meters_t delayed = {
        .rig = CAT_RIG_FTDX10, .po = 10, .swr = 50, .swr_po = 10,
        .po_us = 1000000, .swr_us = 1000000, .swr_po_us = 1000000,
    };
    assert(tx_swr_check(&guard, &delayed, 1000000, 1700000, 300) == TX_SWR_OK);
    assert(tx_swr_check(&guard, &delayed, 1000000, 2100001, 300) == TX_SWR_STALE);
    assert(sample(&guard, 126, 10, 2150000) == TX_SWR_LIMIT);

    // A new high PO sample cannot unlock an old SWR sample taken at low PO.
    guard = (tx_swr_guard_t){0};
    cat_tx_meters_t unmatched = {
        .rig = CAT_RIG_FTDX10, .po = 20, .swr = 126, .swr_po = 1,
        .po_us = 1700000, .swr_us = 1600000, .swr_po_us = 1600000
    };
    assert(tx_swr_check(&guard, &unmatched, 1000000, 1700000, 300) == TX_SWR_OK);
    unmatched.swr_po = 20;
    unmatched.swr_po_us = 1700000;
    unmatched.swr_us = 1700001;
    assert(tx_swr_check(&guard, &unmatched, 1000000, 1700001, 300) == TX_SWR_LIMIT);

    // The profile is known before the first meter reply arrives.
    guard = (tx_swr_guard_t){.rig = CAT_RIG_FTX1};
    const cat_tx_meters_t no_replies = {0};
    assert(tx_swr_check(&guard, &no_replies, 1000000, 3000000, 300) == TX_SWR_OK);
    assert(tx_swr_check(&guard, &no_replies, 1000000, 3000001, 300) == TX_SWR_STALE);
    delayed.rig = CAT_RIG_FTX1;
    assert(tx_swr_check(&guard, &delayed, 1000000, 2100001, 300) == TX_SWR_OK);
    assert(tx_swr_check(&guard, &delayed, 1000000, 3000001, 300) == TX_SWR_STALE);
    delayed.swr = 242;
    assert(tx_swr_check(&guard, &delayed, 1000000, 1000001, 300) == TX_SWR_LIMIT);

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
