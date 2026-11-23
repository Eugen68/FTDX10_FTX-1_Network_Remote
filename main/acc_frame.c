/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Checking and reading one FTDX10 ACC scope frame (see acc_scope.c for the layout). No hardware access,
 * so it is tested on a PC with frames captured from a real FTDX10 (test/host_acc).
 */

#include <string.h>
#include "acc_frame.h"

#define CAT_MATCH_HZ      2000        // the frame's VFO and CAT's may differ this much while tuning
#define CAT_MATCH_HOLD_MS 2000        // and disagree this long before frames are dropped

static const uint32_t s_spans[10] = {1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/**
 * Check one transfer and fill a row from it. cat_hz is CAT's MAIN frequency (0 if unknown); *last_match_ms
 * keeps when the frame's VFO last agreed with it. Returns NULL when accepted, else the reason.
 * Kept free of hardware access so it can be tested on a PC.
 */
const char *acc_parse_frame(const uint8_t *f, size_t len, uint32_t cat_hz, int64_t now_ms,
                            int64_t *last_match_ms, acc_scope_row_t *out)
{
    if (len != ACC_FRAME_BYTES) {
        return "length is not 4096";
    }
    for (size_t i = 3050; i < 3102; i++) {
        if (f[i] != 0) {
            return "not an FTDX10 frame (data after byte 3049)";
        }
    }
    const uint32_t vfo = be32(f + 3038);
    if (vfo != be32(f + 3042)) {
        return "the two VFO copies differ";
    }
    if (vfo < 30000 || vfo > 75000000) {
        return "VFO out of the FTDX10's range";
    }
    const uint8_t span_code = f[3030] & 0x0f;
    const uint8_t placement = (f[3030] >> 6) & 0x03;
    if (span_code > 9 || placement > ACC_PLACE_FIX) {
        return "unknown span or placement";
    }
    if (cat_hz != 0) {
        // While tuning, CAT and the frame can disagree for a moment; a lasting difference means the
        // frame does not belong to this radio's MAIN VFO (or the data is corrupt)
        const uint32_t diff = (vfo > cat_hz) ? vfo - cat_hz : cat_hz - vfo;
        if (*last_match_ms < 0) {
            *last_match_ms = now_ms;           // grace period from the first frame
        }
        if (diff <= CAT_MATCH_HZ) {
            *last_match_ms = now_ms;
        } else if (now_ms - *last_match_ms > CAT_MATCH_HOLD_MS) {
            return "VFO differs from CAT";
        }
    }
    const uint32_t span = s_spans[span_code];
    const uint32_t left = be32(f + 3046);
    out->vfo_hz = vfo;
    out->span_hz = span;
    out->left_hz = left;
    out->marker_hz = be32(f + 3034);
    out->placement = placement;
    out->scope_mode = f[2917];
    // Centre on point 425: the VFO in Center, the left edge plus half the span in Cursor and Fix
    out->center_hz = (placement == ACC_PLACE_CENTER || left == 0) ? vfo : left + span / 2;
    for (int i = 0; i < ACC_SCOPE_POINTS; i++) {
        out->level[i] = (uint8_t)(255 - f[i]);
    }
    return NULL;
}

