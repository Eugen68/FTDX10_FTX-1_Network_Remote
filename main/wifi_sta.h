/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect to the Wi-Fi access point stored in NVS and wait for an IP address
 *
 * The host name for DHCP comes from app_config_t.hostname (default "ftx-1c").
 *
 * NVS must be initialised before calling this function. After the first successful
 * connection the station keeps reconnecting automatically.
 *
 * @return ESP_OK once an IP address was obtained, ESP_ERR_TIMEOUT otherwise
 */
esp_err_t wifi_sta_connect(void);

#ifdef __cplusplus
}
#endif
