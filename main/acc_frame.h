/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/* Checking and reading one FTDX10 ACC scope frame (acc_frame.c) */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "acc_scope.h"

#define ACC_FRAME_BYTES 4096

/**
 * @brief Check one transfer and fill a row from it.
 * @param cat_hz        CAT's MAIN frequency, 0 if unknown
 * @param last_match_ms When the frame's VFO last agreed with CAT; start with -1
 * @return NULL when accepted, else the reason it was rejected
 */
const char *acc_parse_frame(const uint8_t *f, size_t len, uint32_t cat_hz, int64_t now_ms,
                            int64_t *last_match_ms, acc_scope_row_t *out);
