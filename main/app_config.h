/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_SSID_MAX_LEN     32
#define APP_CONFIG_PASS_MAX_LEN     64
#define APP_CONFIG_OTA_USER_MAX_LEN 32
#define APP_CONFIG_OTA_PASS_MAX_LEN 64
#define APP_CONFIG_WEB_USER_MAX_LEN 32
#define APP_CONFIG_WEB_PASS_MAX_LEN 64
#define APP_CONFIG_HOSTNAME_MAX_LEN 63   /* one DNS label */

/** Host name used while none is stored in NVS (reachable as https://ftx-1c.local/) */
#define APP_CONFIG_DEFAULT_HOSTNAME "ftx-1c"

/** Shortest accepted OTA / web password when it is entered on the console */
#define APP_CONFIG_MIN_SECRET_LEN   12

/**
 * Application persistent runtime configuration stored in NVS.
 *
 * web_user / web_pass protect the web UI, the audio streams and /cat (HTTP Basic over HTTPS).
 * While web_pass is empty the web UI falls back to the OTA credentials; with both empty it refuses
 * every request.
 */
typedef struct {
    char wifi_ssid[APP_CONFIG_SSID_MAX_LEN + 1];
    char wifi_pass[APP_CONFIG_PASS_MAX_LEN + 1];
    char ota_user[APP_CONFIG_OTA_USER_MAX_LEN + 1];
    char ota_pass[APP_CONFIG_OTA_PASS_MAX_LEN + 1];
    char web_user[APP_CONFIG_WEB_USER_MAX_LEN + 1];
    char web_pass[APP_CONFIG_WEB_PASS_MAX_LEN + 1];
    char hostname[APP_CONFIG_HOSTNAME_MAX_LEN + 1];   /* DHCP / mDNS host name, without ".local" */
} app_config_t;

/**
 * @brief Initialize config manager and load current settings from NVS (or fall back to Kconfig defaults)
 */
esp_err_t app_config_init(void);

/**
 * @brief Load configuration from NVS. If a field is not found in NVS, Kconfig default is used.
 */
esp_err_t app_config_load(app_config_t *cfg);

/**
 * @brief Save configuration to NVS and update in-memory active config.
 */
esp_err_t app_config_save(const app_config_t *cfg);

/**
 * @brief Clear configuration from NVS (reset to defaults).
 */
esp_err_t app_config_erase(void);

/**
 * @brief Get a copy of the current active configuration.
 */
void app_config_get(app_config_t *cfg);

/**
 * @brief Check if the configuration has a valid Wi-Fi SSID configured.
 */
bool app_config_is_valid(const app_config_t *cfg);

/**
 * @brief Check that s is a valid host name label: 1..63 chars of a-z, 0-9 and '-', not starting or ending with '-'.
 */
bool app_config_hostname_is_valid(const char *s);

#ifdef __cplusplus
}
#endif
