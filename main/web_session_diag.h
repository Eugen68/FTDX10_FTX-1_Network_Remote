/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include "esp_https_server.h"

typedef struct {
    unsigned opened, closed, lru_purges, handshake_errors;
} web_session_stats_t;

web_session_stats_t web_session_stats(void);
void web_session_tls_event(esp_https_server_user_cb_arg_t *event);
void web_session_request(httpd_req_t *req);
void web_session_close_reason(httpd_req_t *req, const char *reason);
esp_err_t web_session_unknown_route(httpd_req_t *req, httpd_err_code_t error);
