/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "tx_swr.h"

unsigned tx_swr_value(cat_rig_t rig, unsigned raw)
{
    // Tables follow each radio's SWR indicator scale (docs/tx-plan.md, requirement 1.10).
    // The radio's Hi-SWR protection remains the first line of defense.
    static const unsigned ftx1[][2] = {{12,100}, {39,135}, {65,150}, {89,200}, {242,500}};
    static const unsigned ftdx10[][2] = {{0,100}, {26,120}, {52,150}, {89,200}, {126,300},
                                         {173,400}, {236,500}, {255,2500}};
    const unsigned (*points)[2];
    size_t count;
    if (rig == CAT_RIG_FTX1) {
        points = ftx1;
        count = sizeof(ftx1) / sizeof(ftx1[0]);
    } else if (rig == CAT_RIG_FTDX10) {
        points = ftdx10;
        count = sizeof(ftdx10) / sizeof(ftdx10[0]);
    } else {
        return 0;
    }
    if (raw <= points[0][0]) {
        return points[0][1];
    }
    for (size_t i = 1; i < count; ++i) {
        if (raw <= points[i][0]) {
            const unsigned span = points[i][0] - points[i-1][0];
            return points[i-1][1] + ((raw - points[i-1][0]) *
                    (points[i][1] - points[i-1][1]) + span / 2) / span;
        }
    }
    return points[count-1][1];
}

int64_t tx_meter_max_age_us(cat_rig_t rig)
{
    return rig == CAT_RIG_FTX1 ? 2000000LL : 1100000LL;
}

tx_swr_result_t tx_swr_check(tx_swr_guard_t *g, const cat_tx_meters_t *m,
                             int64_t started, int64_t now, unsigned swr_max)
{
    const int64_t max_age_us = tx_meter_max_age_us(g->rig);
    if (m->po_us < started || now - m->po_us > max_age_us) {
        // Once the start-up grace expires, a missing PO value cannot prove low power.
        return now - started > max_age_us ? TX_SWR_STALE : TX_SWR_OK;
    }
    if (m->po < TX_PO_MIN_RAW) {
        g->power_since_us = 0;
        g->window_count = 0;
        g->window_pos = 0;
        if (g->reference_count < 3) {
            g->reference_count = 0;
            g->reference_sum = 0;
        }
        if (m->swr_us > g->last_swr_us) {
            g->last_swr_us = m->swr_us;
        }
        return TX_SWR_OK;
    }
    if (!g->power_since_us) {
        g->power_since_us = m->po_us;
    }
    if (m->swr_po_us < g->power_since_us || m->swr_po < TX_PO_MIN_RAW ||
        now - m->swr_us > max_age_us) {
        return now - g->power_since_us > max_age_us ||
               (m->swr_po_us >= g->power_since_us && now - m->swr_us > max_age_us)
               ? TX_SWR_STALE : TX_SWR_OK;
    }
    const unsigned swr = tx_swr_value(m->rig, m->swr);
    if (!swr) {
        return TX_SWR_STALE;
    }
    if (swr >= swr_max) {
        return TX_SWR_LIMIT;
    }
    if (m->swr_us <= g->last_swr_us) {
        return TX_SWR_OK;
    }
    g->last_swr_us = m->swr_us;
    if (g->reference_count < 3) {
        if (m->swr_us - g->power_since_us >= 1000000LL) {
            g->reference_sum += swr;
            ++g->reference_count;
        }
        return TX_SWR_OK;
    }
    g->window[g->window_pos] = swr;
    g->window_pos = (g->window_pos + 1) % 3;
    if (g->window_count < 3) {
        ++g->window_count;
    }
    return g->window_count == 3 && g->window[0] + g->window[1] + g->window[2] >= g->reference_sum + 150
           ? TX_SWR_RISE : TX_SWR_OK;
}
