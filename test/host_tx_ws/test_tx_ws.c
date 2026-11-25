/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "cat.h"
#include "esp_log.h"
#include "tx.h"
#include "tx_voice.h"

typedef struct {
    void *sess_ctx;
    void (*free_ctx)(void *);
} httpd_req_t;

enum { HTTPD_WS_TYPE_TEXT, HTTPD_WS_TYPE_BINARY };
typedef struct {
    unsigned type;
    bool final;
    size_t len;
    uint8_t *payload;
} httpd_ws_frame_t;

static const char *message;
static uint8_t binary[TX_VOICE_FRAME_BYTES] = {3, 2};
static bool sending_binary;
static uint32_t voice_owner;
static char reply[512];
static unsigned auth_checks, key_requests;
static uint32_t acquired_owner, released_owner;
static bool mode_refused;

static esp_err_t httpd_ws_recv_frame(httpd_req_t *req, httpd_ws_frame_t *frame, size_t size)
{
    (void)req;
    if (!size) {
        frame->type = sending_binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT;
        frame->len = sending_binary ? sizeof(binary) : strlen(message);
        frame->final = true;
    } else if (sending_binary) {
        assert(size == sizeof(binary));
        memcpy(frame->payload, binary, size);
    } else {
        assert(size == strlen(message));
        memcpy(frame->payload, message, size);
    }
    return ESP_OK;
}

static esp_err_t httpd_ws_send_frame(httpd_req_t *req, httpd_ws_frame_t *frame)
{
    (void)req;
    assert(frame->len < sizeof(reply));
    memcpy(reply, frame->payload, frame->len);
    reply[frame->len] = 0;
    return ESP_OK;
}

static bool web_auth_ws_check(httpd_req_t *req) { return req != NULL; }
static bool web_auth_tx_check(httpd_req_t *req, const char *pass)
{
    (void)req;
    auth_checks++;
    return strcmp(pass, "good-password") == 0;
}

bool tx_acquire(uint32_t owner)
{
    acquired_owner = owner;
    return true;
}
bool tx_key_tone(uint32_t owner, tx_tone_t tone, int dbfs)
{
    assert(owner == acquired_owner);
    assert(tone == TX_TONE_SINGLE && dbfs == -24);
    key_requests++;
    return !mode_refused;
}
void tx_release(uint32_t owner, const char *reason)
{
    assert(reason != NULL);
    released_owner = owner;
}
bool tx_submit_voice(uint32_t owner, const uint8_t *frame, size_t length)
{
    assert(length == TX_VOICE_FRAME_BYTES && frame[0] == 3 && frame[1] == 2);
    voice_owner = owner;
    return true;
}
void tx_heartbeat(uint32_t owner) { (void)owner; }
tx_state_t tx_get_state(void)
{
    return (tx_state_t){.reason = mode_refused ? "Set MAIN mode to USB, LSB, DATA-USB or DATA-LSB" : ""};
}
bool usb_audio_rx_paused(void) { return false; }
void cat_get_state(cat_state_t *state) { *state = (cat_state_t){0}; }

#include "tx_ws_handler.inc"

static void http_marker_closed(void *ctx) { (void)ctx; }

int main(void)
{
    // note_session() uses a one-byte marker with a different destructor.
    char http_marker = 1;
    httpd_req_t req = {.sess_ctx = &http_marker, .free_ctx = http_marker_closed};
    assert(tx_ws_pre_handshake(&req) == ESP_OK);
    message = "{\"t\":\"ptt\",\"on\":true}";
    assert(tx_ws_handler(&req) == ESP_FAIL);
    assert(key_requests == 0 && acquired_owner == 0 && auth_checks == 0);
    assert(req.sess_ctx == &http_marker && req.free_ctx == http_marker_closed);
    sending_binary = true;
    assert(tx_ws_handler(&req) == ESP_FAIL && voice_owner == 0);
    sending_binary = false;

    message = "{\"t\":\"tx_auth\",\"pass\":\"wrong-password\"}";
    assert(tx_ws_handler(&req) == ESP_OK);
    assert(auth_checks == 1 && strstr(reply, "\"auth\":false"));
    assert(req.sess_ctx == &http_marker && req.free_ctx == http_marker_closed);

    message = "{\"t\":\"tx_auth\",\"pass\":\"good-password\"}";
    assert(tx_ws_handler(&req) == ESP_OK);
    assert(auth_checks == 2 && strstr(reply, "\"auth\":true"));
    assert(req.sess_ctx != &http_marker && req.free_ctx == free_tx_session);
    assert(tx_ws_handler(&req) == ESP_FAIL);
    assert(auth_checks == 2);

    mode_refused = true;
    message = "{\"t\":\"ptt\",\"on\":true,\"tone\":\"single\",\"dbfs\":-24}";
    assert(tx_ws_handler(&req) == ESP_OK);
    assert(key_requests == 1 && acquired_owner != 0);
    assert(strstr(reply, "\"error\":\"Set MAIN mode to USB, LSB, DATA-USB or DATA-LSB\""));
    message = "{\"t\":\"ptt\",\"on\":true,\"tone\":\"dual\",\"dbfs\":0}";
    assert(tx_ws_handler(&req) == ESP_FAIL && key_requests == 1);
    message = "{\"t\":\"ptt\",\"on\":true,\"tone\":\"dual\",\"dbfs\":-06}";
    assert(tx_ws_handler(&req) == ESP_FAIL && key_requests == 1);
    message = "{\"t\":\"ptt\",\"on\":false,\"tone\":\"dual\"}";
    assert(tx_ws_handler(&req) == ESP_FAIL && key_requests == 1);
    sending_binary = true;
    assert(tx_ws_handler(&req) == ESP_OK && voice_owner == acquired_owner && voice_owner != 0);
    sending_binary = false;
    req.free_ctx(req.sess_ctx);
    assert(released_owner == acquired_owner);
    puts("WebSocket session and PTT refusal tests passed");
}
