/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

/*
 * FT8 QSO log entries: parse the JSON the page posts to /log, check every field and format one ADIF
 * record. No I/O here, so the host test can cover it (test/host_qso_log).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QSO_ADIF_JSON_BYTES   384   // largest accepted POST /log body, including the NUL
#define QSO_ADIF_RECORD_BYTES 384   // one formatted record, including "<EOR>\n" and the NUL

typedef struct {
    char call[12], grid[5];
    char my_call[12], my_grid[5];
    char rst_sent[4], rst_rcvd[4];   // "-10", "+05"
    char time_on[15], time_off[15];  // UTC "YYYYMMDDHHMMSS"
    uint32_t freq_hz;                // dial + audio offset of our transmission
} qso_adif_t;

/** Parse and check {"call","grid","sent","rcvd","freq","on","off","mycall","mygrid"}. */
bool qso_adif_parse(const char *json, qso_adif_t *qso);

/** ADIF band name ("20m") for a frequency, or NULL outside the amateur bands both radios cover. */
const char *qso_adif_band(uint32_t freq_hz);

/** One ADIF record on a single line ending in "<EOR>\n". Returns its length, or -1 if it does not fit. */
int qso_adif_format(const qso_adif_t *qso, char *out, size_t capacity);

/** The ADIF file header, ending in "<EOH>\n". Returns its length, or -1 if it does not fit. */
int qso_adif_header(const char *program_version, char *out, size_t capacity);
