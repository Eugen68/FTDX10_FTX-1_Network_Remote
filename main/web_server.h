/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
 *  - GET /stream.pcm  Raw little-endian PCM (format in X-* response headers)
 *  - GET /stream.wav  WAV container with open-ended length
 *
 * Only one stream client is served at a time.
 */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
