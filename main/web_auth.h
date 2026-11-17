/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEB_AUTH_REALM_WEB,   /*!< web UI, audio streams, /status and /cat: web credentials from NVS */
    WEB_AUTH_REALM_OTA,   /*!< POST /ota: OTA credentials from NVS */
} web_auth_realm_t;

/** A state changing request must carry this header. A browser only adds it to a cross-origin request
 *  after a CORS preflight, which this server never grants, so other sites cannot forge such requests. */
#define WEB_AUTH_CSRF_HEADER "X-Requested-With"
#define WEB_AUTH_CSRF_VALUE  "ftx1"

/**
 * @brief Authenticate a request (HTTP Basic over HTTPS)
 *
 * - The web realm uses the web user/password; while no web password is set it falls back to the OTA
 *   credentials. With neither set every request is refused (403).
 * - Failed attempts are counted per client address. After 5 failures the address is locked out for
 *   60 s (429), and every further failure after that locks it again.
 * - For a state changing request (state_changing = true) the request must also carry
 *   WEB_AUTH_CSRF_HEADER, and its Origin / Sec-Fetch-Site headers, when present, must say same-origin.
 *
 * On failure the response has already been sent: the caller just returns ESP_OK.
 * On success the caller should call web_auth_set_common_headers() before it responds.
 *
 * Must be called from the HTTP server task (the lock-out table is not locked).
 *
 * @return true if the request may proceed
 */
bool web_auth_check(httpd_req_t *req, web_auth_realm_t realm, bool state_changing);

/** @brief Add X-Content-Type-Options, X-Frame-Options and Referrer-Policy to the response */
void web_auth_set_common_headers(httpd_req_t *req);

/** @brief Add a Content-Security-Policy suited to the embedded pages (index.html, map.html) */
void web_auth_set_html_headers(httpd_req_t *req);

/** @brief Log which credentials protect the web UI (warns when that is not a dedicated password) */
void web_auth_log_startup(void);

#ifdef __cplusplus
}
#endif
