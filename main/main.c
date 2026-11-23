/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Entry point (app_main). Starts everything in order: NVS and saved settings, the serial setup menu
 * (3 s window at boot), USB audio, CAT, Wi-Fi, the HTTPS server, and finally mDNS (https://<host name>.local/).
 */

#include "esp_log.h"
#include "app_config.h"
#include "acc_scope.h"
#include "cat.h"
#include "tx.h"
#include "console_menu.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "ota.h"
#include "usb_audio.h"
#include "web_server.h"
#include "wifi_sta.h"

static const char *TAG = "main";

void app_main(void)
{
    // Ensure stdout is unbuffered so messages appear immediately
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\r\n\r\n========================================\r\n");
    printf("   FTX-1 Companion app_main started!   \r\n");
    printf("========================================\r\n\r\n");
    fflush(stdout);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(app_config_init());

    // Check UART console for interactive menu or unconfigured Wi-Fi (3s timeout)
    if (!console_menu_check_and_run(3)) {
        ESP_LOGE(TAG, "Wi-Fi is not configured. Halting boot.");
        return;
    }

    ESP_ERROR_CHECK(tx_init());
    ESP_ERROR_CHECK(usb_audio_init());
    ESP_ERROR_CHECK(cat_init());   // after usb_audio_init(): that installs the USB Host Library
    ESP_ERROR_CHECK(acc_scope_init());   // waits for CAT to identify an FTDX10 before touching any pin

    if (wifi_sta_connect() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection failed. Please check Wi-Fi credentials in serial menu.");
        return;
    }
    ESP_ERROR_CHECK(web_server_start());
    ota_confirm_running();   // reached only when Wi-Fi and the web server came up

    // Advertise https://<hostname>.local/ on the LAN (default: ftx-1c.local)
    app_config_t cfg;
    app_config_get(&cfg);
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(cfg.hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set("FTX-1 USB Audio"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_https", "_tcp", 443, NULL, 0));
    ESP_LOGI(TAG, "Open https://%s.local/", cfg.hostname);
}
