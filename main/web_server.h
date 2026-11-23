/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * The HTTPS server (see web_server.c): start it and register all URLs.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP server that streams the USB audio input
 *
 * Endpoints:
 *  - GET /            Web player
 *  - GET /status      JSON stream status
 *  - GET /stream.pcm  Raw little-endian PCM (format in X-* response headers). ?rate=24000 or ?rate=16000
 *                     streams at a lower rate (low-pass filtered), for a weak Wi-Fi link
 *  - GET /stream.wav  WAV container with open-ended length
 *  - GET /catlog      The recent CAT traffic (?since=N), for the page's CAT LOG panel
 *  - GET /scope.bin   The FTDX10's ACC scope rows as a binary stream (?fps=1-25)
 *
 * Only one stream client is served at a time.
 */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
