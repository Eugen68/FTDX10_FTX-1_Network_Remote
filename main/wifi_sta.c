/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Wi-Fi station: connects to the access point saved in NVS, sets the DHCP host name, and reconnects
 * automatically when the link drops.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "app_config.h"
#include "wifi_sta.h"

#define GOT_IP_BIT          BIT0
#define CONNECT_TIMEOUT_MS  30000

static const char *TAG = "wifi_sta";
static EventGroupHandle_t s_event_group;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // Create the IPv6 link-local address so mDNS can answer AAAA queries.
        // Without it macOS waits ~5 s for an AAAA reply before using the A record.
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_create_ip6_linklocal(netif);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, reconnecting");
        xEventGroupClearBits(s_event_group, GOT_IP_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_event_group, GOT_IP_BIT);
    }
}

esp_err_t wifi_sta_connect(void)
{
    s_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_event_group, ESP_ERR_NO_MEM, TAG, "no memory");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "create netif");
    app_config_t cfg;
    app_config_get(&cfg);
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(netif, cfg.hostname), TAG, "set hostname");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL), TAG, "register");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL), TAG, "register");

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, cfg.wifi_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, cfg.wifi_pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = strlen(cfg.wifi_pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config");
    // 20 MHz channel instead of 40 MHz: the audio stream needs about 1.5 Mbit/s, far below what 20 MHz
    // carries, and on a busy 2.4 GHz band a 40 MHz channel meets more interference and retries, which
    // showed as send stalls of several seconds. Not fatal if the driver refuses it.
    esp_err_t bw_err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    if (bw_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set the 20 MHz bandwidth: %s", esp_err_to_name(bw_err));
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    // Real-time audio: disable modem sleep, otherwise latency jitters by up to a few hundred ms
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable power save");

    ESP_LOGI(TAG, "Connecting to SSID \"%s\"", cfg.wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_event_group, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    ESP_RETURN_ON_FALSE(bits & GOT_IP_BIT, ESP_ERR_TIMEOUT, TAG, "Wi-Fi connection timed out");
    return ESP_OK;
}
