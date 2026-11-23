/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Host test for main/acc_frame.c with frames captured from a real FTDX10 (Eugene YO9VA, 2026-11-20, see
 * captures/README). Each capture is the ACC pipe board's packet stream: 'Y' 'A' type flags len(LE16)
 * seq(LE16), then the payload; type 'F' is a frame. Run: test/host_acc/run.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acc_frame.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const char *file; uint32_t cat_hz; uint32_t span; uint8_t placement; } cap_t;

static int run_capture(const char *dir, const cap_t *c, uint8_t *first_frame)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, c->file);
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("cannot open %s\n", path); fails++; return 0; }
    static uint8_t buf[2 * 1024 * 1024];
    const size_t n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    int frames = 0, accepted = 0;
    int64_t last = -1;
    acc_scope_row_t row;
    for (size_t i = 0; i + 8 <= n;) {
        if (buf[i] != 'Y' || buf[i + 1] != 'A') { i++; continue; }
        const size_t len = buf[i + 4] | (buf[i + 5] << 8);
        if (i + 8 + len > n) break;
        if (buf[i + 2] == 'F') {
            const uint8_t *f = buf + i + 8;
            if (frames == 0 && first_frame) memcpy(first_frame, f, len < 4096 ? len : 4096);
            const char *why = acc_parse_frame(f, len, c->cat_hz, frames * 40, &last, &row);
            frames++;
            if (why == NULL) {
                accepted++;
                CHECK(row.span_hz == c->span && row.placement == c->placement, "%s: span %u placement %u",
                      c->file, row.span_hz, row.placement);
                CHECK(row.vfo_hz == c->cat_hz, "%s: vfo %u", c->file, row.vfo_hz);
                const uint32_t lo = row.center_hz - row.span_hz / 2, hi = row.center_hz + row.span_hz / 2;
                CHECK(row.vfo_hz >= lo && row.vfo_hz <= hi, "%s: VFO %u outside the span %u-%u", c->file, row.vfo_hz, lo, hi);
            } else {
                printf("  %s frame %d rejected: %s\n", c->file, frames, why);
            }
        }
        i += 8 + len;
    }
    printf("%-32s frames %3d accepted %3d\n", c->file, frames, accepted);
    CHECK(frames > 50 && accepted == frames, "%s: %d of %d accepted", c->file, accepted, frames);
    return accepted;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "captures";
    const cap_t caps[] = {
        {"ftdx10-14074-20k.bin", 14074000, 20000, ACC_PLACE_CENTER},
        {"ftdx10-14074-20k-cursor.bin", 14074000, 20000, ACC_PLACE_CURSOR},
        {"ftdx10-7074-100k-fix.bin", 7074000, 100000, ACC_PLACE_FIX},
        {"ftdx10-14074-1m.bin", 14074000, 1000000, ACC_PLACE_CENTER},
    };
    static uint8_t f[4096];
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) run_capture(dir, &caps[i], i == 0 ? f : NULL);

    // Rejections, starting from a real frame
    acc_scope_row_t row;
    int64_t last = -1;
    CHECK(acc_parse_frame(f, 4095, 14074000, 0, &last, &row) != NULL, "short frame accepted");
    uint8_t g[4096];
    memcpy(g, f, sizeof g); g[3060] = 1;
    last = -1;
    CHECK(acc_parse_frame(g, 4096, 14074000, 0, &last, &row) != NULL, "data after 3049 accepted");
    memcpy(g, f, sizeof g); g[3041] ^= 1;
    last = -1;
    CHECK(acc_parse_frame(g, 4096, 14074000, 0, &last, &row) != NULL, "differing VFO copies accepted");
    // CAT disagreement: accepted during the grace period, rejected after 2 s
    last = -1;
    CHECK(acc_parse_frame(f, 4096, 7074000, 0, &last, &row) == NULL, "grace period");
    CHECK(acc_parse_frame(f, 4096, 7074000, 1500, &last, &row) == NULL, "still in grace");
    CHECK(acc_parse_frame(f, 4096, 7074000, 2500, &last, &row) != NULL, "lasting CAT mismatch accepted");
    CHECK(acc_parse_frame(f, 4096, 14074500, 2600, &last, &row) == NULL, "match again (within 2 kHz) rejected");
    CHECK(acc_parse_frame(f, 4096, 0, 9000, &last, &row) == NULL, "CAT unknown should not reject");
    // Levels are inverted
    CHECK(row.level[0] == 255 - f[0] && row.level[849] == 255 - f[849], "inversion");

    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
