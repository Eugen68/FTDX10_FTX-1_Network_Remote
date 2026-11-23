/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/* Checking and reading one FTDX10 ACC scope frame (acc_frame.c) */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "acc_scope.h"

#define ACC_FRAME_BYTES 4096

/** @brief Why a frame was rejected (0 = accepted) */
typedef enum {
    ACC_OK = 0,
    ACC_REJ_LENGTH,           /*!< not 4096 bytes (e.g. the first transfer, caught half way) */
    ACC_REJ_NOT_FTDX10,       /*!< data after byte 3049 */
    ACC_REJ_VFO_COPIES,       /*!< the two VFO copies differ (bit errors) */
    ACC_REJ_VFO_RANGE,        /*!< VFO outside 30 kHz - 75 MHz */
    ACC_REJ_SPAN,             /*!< unknown span or placement code */
    ACC_REJ_CAT,              /*!< VFO differs from CAT for more than 2 s */
    ACC_REJ_COUNT
} acc_reject_t;

/** @brief Short text for a reason code */
const char *acc_reject_text(acc_reject_t r);

/**
 * @brief Check one transfer and fill a row from it.
 * @param cat_hz        CAT's MAIN frequency, 0 if unknown
 * @param last_match_ms When the frame's VFO last agreed with CAT; start with -1
 * @return ACC_OK when accepted, else the reason it was rejected
 */
acc_reject_t acc_parse_frame(const uint8_t *f, size_t len, uint32_t cat_hz, int64_t now_ms,
                            int64_t *last_match_ms, acc_scope_row_t *out);
