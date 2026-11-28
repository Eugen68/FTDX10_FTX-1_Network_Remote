/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "cat.h"
#include "esp_log.h"
#include "tx.h"
#include "ft8_tx.h"
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
static uint8_t binary[TX_VOICE_PACKET_BYTES] = {3, 2};
static bool sending_binary;
static unsigned receive_yields;
static void vTaskDelay(unsigned ticks) { assert(ticks == 1); ++receive_yields; }
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
    assert(length == TX_VOICE_PACKET_BYTES && frame[0] == 3 && frame[1] == 2);
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

static unsigned power_calls, power_watts;
esp_err_t cat_set_tx_power(unsigned watts)
{
    ++power_calls;
    power_watts = watts;
    return watts >= 5 && watts <= 100 ? ESP_OK : ESP_ERR_INVALID_ARG;
}
static unsigned ft8_arms, ft8_levels;
void ft8_tx_set_level(uint32_t owner, int dbfs)
{
    assert(owner && dbfs == -50);
    ++ft8_levels;
}
static uint32_t ft8_owner;
int64_t esp_timer_get_time(void) { return 7300000; }
const char *ft8_tx_arm(uint32_t owner, const char *msg, unsigned hz, bool odd, int dbfs)
{
    assert(owner && strcmp(msg, "CQ K1ABC FN42") == 0 && hz == 1500 && odd && dbfs == -24);
    ++ft8_arms;
    ft8_owner = owner;
    return "";
}
void ft8_tx_halt(uint32_t owner, const char *reason)
{
    assert(owner && reason);
    if (ft8_owner == owner) ft8_owner = 0;
}
void ft8_tx_heartbeat(uint32_t owner) { assert(owner); }
void ft8_tx_browser_clock(uint32_t owner, int64_t offset, unsigned rtt)
{
    assert(owner && offset == 1795570000000LL && rtt == 20);
}
ft8_tx_status_t ft8_tx_status(uint32_t owner)
{
    return (ft8_tx_status_t){.owned = owner && owner == ft8_owner, .clock = "none", .reason = ""};
}

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
    assert(tx_ws_handler(&req) == ESP_OK && voice_owner == acquired_owner && voice_owner != 0 && receive_yields == 1);
    sending_binary = false;
    req.free_ctx(req.sess_ctx);
    assert(released_owner == acquired_owner);
    httpd_req_t ft8_req = {0};
    // TX power only over an authorized TX connection
    message = "{\"t\":\"power\",\"w\":50}";
    assert(tx_ws_handler(&ft8_req) == ESP_FAIL && power_calls == 0);
    message = "{\"t\":\"ft8_arm\",\"msg\":\"CQ K1ABC FN42\",\"hz\":1500,\"odd\":true}";
    assert(tx_ws_handler(&ft8_req) == ESP_FAIL && ft8_arms == 0);
    message = "{\"t\":\"tx_auth\",\"pass\":\"good-password\"}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK);
    message = "{\"t\":\"ft8_ping\"}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK && strstr(reply, "\"mono\":7300"));
    message = "{\"t\":\"ft8_clock\",\"offset\":1795570000000,\"rtt\":20}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK);
    message = "{\"t\":\"ft8_arm\",\"msg\":\"CQ K1ABC FN42\",\"hz\":1500,\"odd\":true}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK && ft8_arms == 1 && strstr(reply, "\"ft8_owned\":true"));
    message = "{\"t\":\"ft8_level\",\"dbfs\":-50}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK && ft8_levels == 1);
    message = "{\"t\":\"ft8_halt\"}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK && !ft8_owner);
    message = "{\"t\":\"power\",\"w\":50}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK && power_calls == 1 && power_watts == 50 && !strstr(reply, "Power"));
    message = "{\"t\":\"power\",\"w\":3}";
    assert(tx_ws_handler(&ft8_req) == ESP_OK && power_calls == 2 && strstr(reply, "Power out of range"));
    message = "{\"t\":\"power\",\"w\":101}";
    assert(tx_ws_handler(&ft8_req) == ESP_FAIL && power_calls == 2);
    message = "{\"t\":\"power\",\"w\":50,\"on\":true}";
    assert(tx_ws_handler(&ft8_req) == ESP_FAIL && power_calls == 2);
    ft8_req.free_ctx(ft8_req.sess_ctx);
    puts("WebSocket session and PTT refusal tests passed");
}
