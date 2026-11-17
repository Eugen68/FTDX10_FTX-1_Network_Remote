/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Firmware update over the network (OTA): POST /ota on the HTTPS server receives the new image, writes it
 * to the next app slot and reboots. Protected by the OTA credentials. A new image is rolled back by the
 * bootloader unless ota_confirm_running() is reached after the next boot.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "web_auth.h"
#include "ota.h"

#define OTA_CHUNK_BYTES      4096
#define OTA_MAX_TIMEOUTS     5                    // consecutive 5 s receive timeouts before giving up
#define REBOOT_DELAY         pdMS_TO_TICKS(500)

static const char *TAG = "ota";
static atomic_bool s_busy;

static esp_err_t ota_handler(httpd_req_t *req)
{
    // The body is not always read, so do not reuse the connection
    httpd_resp_set_hdr(req, "Connection", "close");

    // Basic authentication with the OTA credentials, per-client lock-out after repeated failures,
    // and the cross-site request checks (curl: add -H "X-Requested-With: ftx1")
    if (!web_auth_check(req, WEB_AUTH_REALM_OTA, true)) {
        return ESP_OK;
    }
    web_auth_set_common_headers(req);

    const size_t total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
    }
    if (total == 0) {
        return httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "Send the firmware image as the request body");
    }
    if (total > part->size) {
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Image is larger than the OTA partition");
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "An update is already in progress", HTTPD_RESP_USE_STRLEN);
    }

    uint8_t *buf = malloc(OTA_CHUNK_BYTES);
    esp_ota_handle_t handle = 0;
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    bool begun = false;
    if (err == ESP_OK) {
        err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
        begun = (err == ESP_OK);
    }

    size_t received = 0;
    int timeouts = 0;
    while (err == ESP_OK && received < total) {
        size_t want = total - received;
        int n = httpd_req_recv(req, (char *)buf, want < OTA_CHUNK_BYTES ? want : OTA_CHUNK_BYTES);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > OTA_MAX_TIMEOUTS) {
                err = ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(handle, buf, n);
        received += n;
    }
    free(buf);

    if (err == ESP_OK) {
        err = esp_ota_end(handle);   // validates the image (magic, chip, checksum, SHA-256)
    } else if (begun) {
        esp_ota_abort(handle);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Update failed after %u of %u bytes: %s", (unsigned)received, (unsigned)total, esp_err_to_name(err));
        atomic_store(&s_busy, false);
        return httpd_resp_send_err(req, err == ESP_ERR_OTA_VALIDATE_FAILED ? HTTPD_400_BAD_REQUEST
                                                                           : HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Update failed");
    }

    ESP_LOGI(TAG, "Wrote %u bytes to %s, rebooting", (unsigned)total, part->label);
    httpd_resp_send(req, "Update OK, rebooting\n", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(REBOOT_DELAY);   // let the response leave before the connection is dropped
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_register(httpd_handle_t server)
{
    static const httpd_uri_t uri = {.uri = "/ota", .method = HTTP_POST, .handler = ota_handler};
    return httpd_register_uri_handler(server, &uri);
}

void ota_confirm_running(void)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New firmware confirmed, rollback cancelled");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
