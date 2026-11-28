/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "esp_https_server.h"

// The serial menu runs before Wi-Fi. Replace the identity at HTTPS startup.
void https_identity_request_regeneration(void);

// Starts HTTPS with the stored identity, or with the embedded fallback when
// a device identity cannot be loaded or generated.
esp_err_t https_identity_start(httpd_handle_t *server, httpd_ssl_config_t *config);

// Compare the active SAN with the current IPv4 address, including DHCP changes.
bool https_identity_covers_ip(void);
// SHA-256 of the active certificate in colon-separated hex. Empty on parse failure.
const char *https_identity_fingerprint(void);
