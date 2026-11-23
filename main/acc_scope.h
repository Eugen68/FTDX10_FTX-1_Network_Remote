/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * The radio's own spectrum scope, read from the FTDX10's ACC socket (see acc_scope.c): the latest scope row
 * with its tuning, and statistics for /status.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACC_SCOPE_POINTS 850

/** @brief Placement of the radio's scope */
typedef enum {
    ACC_PLACE_CENTER = 0,
    ACC_PLACE_CURSOR = 1,
    ACC_PLACE_FIX = 2,
    ACC_PLACE_UNKNOWN = 3,
} acc_placement_t;

/** @brief One scope row and where it sits */
typedef struct {
    uint32_t seq;                      /*!< Increases by one per accepted frame, 0 = none yet */
    uint32_t ms;                       /*!< Time since boot when it arrived */
    uint32_t vfo_hz;                   /*!< MAIN VFO as sent in the frame */
    uint32_t center_hz;                /*!< Frequency of point 425 */
    uint32_t span_hz;                  /*!< The span the radio shows (the 850 points cover a little more) */
    uint32_t left_hz;                  /*!< Left edge of the span as sent (valid in Cursor and Fix) */
    uint32_t marker_hz;                /*!< The passband marker (VFO + 1500 Hz + IF shift in USB) */
    uint8_t placement;                 /*!< acc_placement_t */
    uint8_t scope_mode;                /*!< The radio's scope mode byte (3DSS / Center / Cursor / Fix) */
    uint8_t level[ACC_SCOPE_POINTS];   /*!< 0 - 255, higher is stronger (the frame's bytes, inverted) */
} acc_scope_row_t;

/** @brief Statistics for /status */
typedef struct {
    bool enabled;                      /*!< The SPI slave runs (an FTDX10 was detected since boot) */
    uint32_t frames;                   /*!< Transfers received */
    uint32_t accepted;                 /*!< Frames that passed every check */
    uint32_t rejected;                 /*!< Frames dropped by a check */
    uint32_t fps;                      /*!< Accepted frames in the last second */
    uint32_t age_ms;                   /*!< Since the last accepted frame (UINT32_MAX = never) */
    uint32_t last_len;                 /*!< Length of the last transfer, bytes */
    char reason[48];                   /*!< Why the last frame was rejected, "" if it was accepted */
} acc_scope_stats_t;

/**
 * @brief Start the background task. It waits until CAT identifies an FTDX10 and only then sets the
 *        ACC pins up as an SPI slave (on any other radio they stay untouched inputs).
 */
esp_err_t acc_scope_init(void);

/** @brief Copy the latest accepted row; false if there is none yet */
bool acc_scope_get_row(acc_scope_row_t *out);

/** @brief Copy the statistics */
void acc_scope_get_stats(acc_scope_stats_t *out);

#ifdef __cplusplus
}
#endif
