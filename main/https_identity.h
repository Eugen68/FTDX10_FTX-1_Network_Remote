/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#pragma once

#include "esp_https_server.h"

// The serial menu runs before Wi-Fi. Replace the identity at HTTPS startup.
void https_identity_request_regeneration(void);

// Starts HTTPS with the stored identity, or with the embedded fallback when
// a device identity cannot be loaded or generated.
esp_err_t https_identity_start(httpd_handle_t *server, httpd_ssl_config_t *config);
