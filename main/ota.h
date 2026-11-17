/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#include "esp_err.h"
#include "esp_https_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register POST /ota on the HTTPS server
 *
 * The request body is the raw application image (build/usb_host_lib.bin). It is protected by HTTP
 * Basic authentication (user "ota", password CONFIG_OTA_PASSWORD) and, with an empty password, OTA is
 * disabled. After a valid image is written, the device reboots into it.
 */
esp_err_t ota_register(httpd_handle_t server);

/**
 * @brief Confirm the running firmware after an OTA update
 *
 * Call it once the firmware has proven it works (Wi-Fi and the web server are up). A new image that
 * is not confirmed is rolled back by the bootloader on the next reboot, e.g. after a crash loop.
 * Does nothing if the running image is already confirmed.
 */
void ota_confirm_running(void);

#ifdef __cplusplus
}
#endif
