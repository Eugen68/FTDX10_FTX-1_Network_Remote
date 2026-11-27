/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned completed, short_packets, failed, submit_failed, bytes;
    size_t sample_len;
    uint8_t sample[16];
} cat_cdc_probe_t;

void cat_cdc_probe_begin(void);
cat_cdc_probe_t cat_cdc_probe_end(void);
