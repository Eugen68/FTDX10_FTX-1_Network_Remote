/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Saved settings (Wi-Fi, OTA / web credentials, host name) kept in NVS, and the functions to read and
 * change them. The defaults and length limits are defined here.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

/** Host name used while none is stored in NVS (reachable as https://trx-remote.local/) */
#define APP_CONFIG_DEFAULT_HOSTNAME "trx-remote"

/** Shortest accepted OTA / web password when it is entered on the console */
#define APP_CONFIG_MIN_SECRET_LEN   12
#define APP_CONFIG_MIN_TX_SECRET_LEN 6
#define RIGCTL_DEFAULT_PORT 4532
#define YAESU_CAT_DEFAULT_PORT 4533
#define TX_DEFAULT_SECONDS 120
#define TX_SWR_DEFAULT 30
#define TX_SWR_MIN 15
#define TX_SWR_MAX 30
#define TX_MIN_SECONDS 10
#define TX_MAX_SECONDS 600

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
    bool rigctl_enabled;
    bool rigctl_debug;
    uint16_t rigctl_port;
    char rigctl_ip[16];      /* allowed client for NET rigctl and the Yaesu CAT server, "" = any */
    bool yaesu_enabled;      /* Yaesu CAT over TCP (OmniRig through a virtual COM port) */
    bool yaesu_debug;
    uint16_t yaesu_port;
    bool tx_enabled;
    bool cat_console;        /* the page's CAT console (typed commands); off by default */
    unsigned tx_max_seconds;
    unsigned swr_max; // tenths of the SWR ratio
    char tx_pass[APP_CONFIG_PASS_MAX_LEN + 1];
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

/** Remote PTT requires the serial-menu setting and a configured TX secret. */
bool app_config_tx_enabled(void);
bool app_config_rigctl_enabled(void);
bool app_config_yaesu_enabled(void);
bool app_config_cat_console_enabled(void);

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
