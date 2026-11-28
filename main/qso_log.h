/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

/*
 * The FT8 QSO log, kept as ADIF records in a file on the "storage" partition (FAT with wear levelling),
 * so it survives reboots and OTA updates. Called from the HTTP server task only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"

#define QSO_LOG_MAX_BYTES (1024 * 1024)   // about 4000 QSOs; POST /log is refused beyond this

/** Mount the partition (formatting it on first use) and count the stored QSOs. */
esp_err_t qso_log_init(void);

/** True once qso_log_init() succeeded. */
bool qso_log_ready(void);

/** Append one formatted ADIF record. ESP_ERR_NO_MEM when the log is full. */
esp_err_t qso_log_append(const char *record);

/** Number of stored QSOs and the size of the record file. */
void qso_log_stats(unsigned *count, size_t *bytes);

/** Open the record file for reading (NULL when the log is empty or not ready); the caller closes it. */
FILE *qso_log_open(void);
