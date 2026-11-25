/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Saved settings (Wi-Fi, OTA / web credentials, host name) kept in NVS: load, save, erase, the default
 * values, and the validity checks (for example the host name rules).
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "app_config.h"

#define NVS_NAMESPACE     "app_cfg"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_OTA_USER  "ota_user"
#define NVS_KEY_OTA_PASS  "ota_pass"
#define NVS_KEY_WEB_USER  "web_user"
#define NVS_KEY_WEB_PASS  "web_pass"
#define NVS_KEY_HOSTNAME  "hostname"

static const char *TAG = "app_config";
static app_config_t s_active_config;
static bool s_initialized = false;

static void read_nvs_str_or_default(nvs_handle_t handle, const char *key, char *out, size_t max_len, const char *fallback)
{
    size_t req_len = max_len;
    esp_err_t err = nvs_get_str(handle, key, out, &req_len);
    if (err != ESP_OK) {
        strlcpy(out, fallback ? fallback : "", max_len);
    }
}

esp_err_t app_config_load(app_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->tx_max_seconds = TX_DEFAULT_SECONDS;
    cfg->swr_max = TX_SWR_DEFAULT;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    const char *def_ssid = "";
    const char *def_pass = "";
    const char *def_ota_user = "ota";
    const char *def_ota_pass = "";
    const char *def_web_user = "admin";
    const char *def_web_pass = "";
    const char *def_hostname = APP_CONFIG_DEFAULT_HOSTNAME;

    if (err == ESP_OK) {
        read_nvs_str_or_default(handle, NVS_KEY_WIFI_SSID, cfg->wifi_ssid, sizeof(cfg->wifi_ssid), def_ssid);
        read_nvs_str_or_default(handle, NVS_KEY_WIFI_PASS, cfg->wifi_pass, sizeof(cfg->wifi_pass), def_pass);
        read_nvs_str_or_default(handle, NVS_KEY_OTA_USER, cfg->ota_user, sizeof(cfg->ota_user), def_ota_user);
        read_nvs_str_or_default(handle, NVS_KEY_OTA_PASS, cfg->ota_pass, sizeof(cfg->ota_pass), def_ota_pass);
        read_nvs_str_or_default(handle, NVS_KEY_WEB_USER, cfg->web_user, sizeof(cfg->web_user), def_web_user);
        read_nvs_str_or_default(handle, NVS_KEY_WEB_PASS, cfg->web_pass, sizeof(cfg->web_pass), def_web_pass);
        read_nvs_str_or_default(handle, NVS_KEY_HOSTNAME, cfg->hostname, sizeof(cfg->hostname), def_hostname);
        uint8_t tx_enabled = 0;
        uint16_t tx_seconds = 0;
        nvs_get_u8(handle, "tx_enabled", &tx_enabled);
        nvs_get_u16(handle, "tx_max_seconds", &tx_seconds);
        uint8_t swr = 0;
        nvs_get_u8(handle, "swr_max", &swr);
        if (swr >= TX_SWR_MIN && swr <= TX_SWR_MAX) {
            cfg->swr_max = swr;
        }
        cfg->tx_enabled = tx_enabled == 1;
        if (tx_seconds >= TX_MIN_SECONDS && tx_seconds <= TX_MAX_SECONDS) {
            cfg->tx_max_seconds = tx_seconds;
        }
        read_nvs_str_or_default(handle, "tx_pass", cfg->tx_pass, sizeof(cfg->tx_pass), "");
        nvs_close(handle);
    } else {
        // NVS namespace not created yet; fall back to defaults
        strlcpy(cfg->wifi_ssid, def_ssid, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_pass, def_pass, sizeof(cfg->wifi_pass));
        strlcpy(cfg->ota_user, def_ota_user, sizeof(cfg->ota_user));
        strlcpy(cfg->ota_pass, def_ota_pass, sizeof(cfg->ota_pass));
        strlcpy(cfg->web_user, def_web_user, sizeof(cfg->web_user));
        strlcpy(cfg->web_pass, def_web_pass, sizeof(cfg->web_pass));
        strlcpy(cfg->hostname, def_hostname, sizeof(cfg->hostname));
    }

    // Treat the old factory default as the new default without rewriting NVS.
    // Valid custom names continue to use their saved value.
    if (!app_config_hostname_is_valid(cfg->hostname) || strcmp(cfg->hostname, "ftx-1c") == 0) {
        strlcpy(cfg->hostname, def_hostname, sizeof(cfg->hostname));
    }

    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_WIFI_SSID, cfg->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_KEY_WIFI_PASS, cfg->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_KEY_OTA_USER, cfg->ota_user);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_KEY_OTA_PASS, cfg->ota_pass);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_KEY_WEB_USER, cfg->web_user);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_KEY_WEB_PASS, cfg->web_pass);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_KEY_HOSTNAME, cfg->hostname);
    if (err == ESP_OK) err = nvs_set_u8(handle, "tx_enabled", cfg->tx_enabled);
    if (err == ESP_OK) err = nvs_set_str(handle, "tx_pass", cfg->tx_pass);
    if (err == ESP_OK) err = nvs_set_u16(handle, "tx_max_seconds", cfg->tx_max_seconds);
    if (err == ESP_OK) err = nvs_set_u8(handle, "swr_max", cfg->swr_max);

    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Configuration committed to NVS");
            memcpy(&s_active_config, cfg, sizeof(s_active_config));
        } else {
            ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Failed to write NVS keys: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t app_config_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            nvs_commit(handle);
            ESP_LOGI(TAG, "NVS configuration erased");
            app_config_load(&s_active_config);
        }
        nvs_close(handle);
    }
    return err;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = app_config_load(&s_active_config);
    if (err == ESP_OK) {
        s_initialized = true;
    }
    return err;
}

void app_config_get(app_config_t *cfg)
{
    if (!cfg) return;
    if (!s_initialized) {
        app_config_load(&s_active_config);
        s_initialized = true;
    }
    memcpy(cfg, &s_active_config, sizeof(app_config_t));
}

bool app_config_hostname_is_valid(const char *s)
{
    if (!s) return false;
    size_t len = strlen(s);
    if (len == 0 || len > APP_CONFIG_HOSTNAME_MAX_LEN || s[0] == '-' || s[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

bool app_config_tx_enabled(void)
{
    return s_active_config.tx_enabled && strlen(s_active_config.tx_pass) >= APP_CONFIG_MIN_TX_SECRET_LEN;
}

bool app_config_is_valid(const app_config_t *cfg)
{
    if (!cfg) return false;
    return (cfg->wifi_ssid[0] != '\0');
}
