/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include "cat.h"

#define TX_PO_MIN_RAW 3

typedef struct {
    cat_rig_t rig;
    int64_t power_since_us;
    int64_t last_swr_us;
    unsigned reference_sum;
    unsigned reference_count;
    unsigned window[3];
    unsigned window_count;
    unsigned window_pos;
} tx_swr_guard_t;

typedef enum { TX_SWR_OK, TX_SWR_STALE, TX_SWR_LIMIT, TX_SWR_RISE } tx_swr_result_t;

/** Convert RM6 to hundredths of an SWR ratio. Zero means unknown. */
unsigned tx_swr_value(cat_rig_t rig, unsigned raw);
/** Applies to both startup grace and the freshness of accepted readings. */
int64_t tx_meter_max_age_us(cat_rig_t rig);
tx_swr_result_t tx_swr_check(tx_swr_guard_t *guard, const cat_tx_meters_t *meters,
                             int64_t started, int64_t now, unsigned swr_max);
