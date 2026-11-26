/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static char warnings[4096];

static void record_warning(const char *format, ...)
{
    const size_t len = strlen(warnings);
    va_list args;
    va_start(args, format);
    vsnprintf(warnings + len, sizeof(warnings) - len, format, args);
    va_end(args);
}

#undef ESP_LOGI
#define ESP_LOGI(tag, ...) record_warning(__VA_ARGS__)
#undef ESP_LOGW
#define ESP_LOGW(tag, ...) record_warning(__VA_ARGS__)
#include "tx.c"

int64_t g_now_us = 1000000;
static int64_t rx_us;
static bool enabled = true;
static bool radio_on;
static bool cat_available = true;
static bool usb_available = true;
static bool output_available = true;
static bool output_idle = true;
static bool defer_audio_cleanup, cleanup_pending;
static cat_rig_t radio_model = CAT_RIG_FTDX10;
static bool prepare_fails, prepare_changes_mode, prepare_shared;
static unsigned preparations, cleanups;
static tx_tone_t audio_request;
static unsigned po_raw, swr_raw;
static bool meters_missing;
static unsigned stale_diagnostics_reads;
static unsigned on_writes, off_writes;
static unsigned voice_frames;
bool tx_voice_start(void) { return true; }
void tx_voice_stop(void) {}
void tx_voice_push(const uint8_t pcm[2 * TX_VOICE_SAMPLES]) { (void)pcm; ++voice_frames; }
static const char *fault;
static const char *mode_error;

const char *cat_ptt_mode_error(int64_t keyed_at) { return mode_error; }
const char *cat_voice_mode_error(bool starting) { return NULL; }
const char *cat_data_mode_error(void) { return NULL; }

bool app_config_tx_enabled(void) { return enabled; }
void app_config_get(app_config_t *cfg) { *cfg = (app_config_t){.tx_max_seconds = 10, .swr_max = 30}; }
bool usb_audio_get_format(usb_audio_format_t *fmt) { return usb_available; }
bool usb_audio_tx_available(void) { return output_available; }
bool usb_audio_tx_idle(void) { return output_idle; }
bool usb_audio_tx_cleanup_pending(void) { return cleanup_pending; }
void usb_audio_tx_request(tx_tone_t tone, int dbfs) { audio_request = tone; }
void cat_get_state(cat_state_t *state) { *state = (cat_state_t){.rig = radio_model}; }
esp_err_t usb_audio_tx_prepare(bool shared)
{
    assert(!radio_on && output_idle);
    ++preparations;
    prepare_shared = shared;
    if (prepare_changes_mode) {
        mode_error = "Mode changed";
    }
    return prepare_fails ? ESP_ERR_INVALID_STATE : ESP_OK;
}
void usb_audio_tx_finished(void)
{
    assert(!radio_on && audio_request == TX_TONE_OFF);
    ++cleanups;
    if (defer_audio_cleanup) {
        cleanup_pending = true;
        output_idle = false;
    }
}
cat_tx_meters_t cat_get_tx_meters(void)
{
    if (meters_missing) {
        return (cat_tx_meters_t){.rig = CAT_RIG_FTDX10};
    }
    return (cat_tx_meters_t){
        .rig = CAT_RIG_FTDX10, .po = po_raw, .swr = swr_raw, .swr_po = po_raw, .po_us = g_now_us,
        .swr_us = g_now_us, .swr_po_us = g_now_us
    };
}
cat_write_stats_t cat_get_write_stats(void)
{
    if (!s_stopping) return (cat_write_stats_t){0};
    assert(off_writes == 10);
    ++stale_diagnostics_reads;
    return (cat_write_stats_t){.meters = {.calls = 6, .timeouts = 1, .max_us = 10000}};
}
size_t cat_log_recent_tx_meters(uint32_t through_ms, cat_log_entry_t *entries, size_t max)
{
    assert(through_ms == (uint32_t)(g_now_us / 1000) && max >= 1);
    *entries = (cat_log_entry_t){.dir = 'T', .text = "RM5;"};
    return 1;
}
bool cat_radio_rx_after(int64_t after_us) { return cat_available && !radio_on && rx_us > after_us; }
bool cat_radio_tx(void) { return cat_available && radio_on; }
const char *cat_ptt_fault(int64_t started, int64_t grace_us) { return cat_available ? fault : "CAT lost"; }
void cat_enable_ft8_alc(void) {}
esp_err_t cat_ptt(bool on)
{
    if (!cat_available) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on) {
        on_writes++;
        radio_on = true;
    } else {
        off_writes++;
    }
    return ESP_OK;
}

static void confirm_rx(void)
{
    radio_on = false;
    rx_us = ++g_now_us;
    watchdog_tick();
    assert(tx_get_state().phase == TX_IDLE);
}

static void key(void)
{
    rx_us = g_now_us;
    assert(tx_acquire(1));
    assert(!tx_acquire(2));
    assert(tx_key(1));
    assert(tx_get_state().on_air);
}

int main(void)
{
    s_lock = xSemaphoreCreateMutex();
    cat_available = false;
    assert(tx_begin_usb_recovery());
    assert(!tx_acquire(1));
    tx_resume();
    cat_available = true;
    radio_on = true;
    assert(!tx_begin_usb_recovery());
    radio_on = false;
    enabled = false;
    assert(!tx_acquire(1));
    enabled = true;

    mode_error = "Set MAIN mode to USB, LSB, DATA-USB or DATA-LSB";
    rx_us = g_now_us;
    assert(tx_acquire(1));
    assert(!tx_key(1));
    assert(tx_get_state().phase == TX_IDLE);
    assert(tx_get_state().reason == mode_error);
    assert(on_writes == 0 && off_writes == 0);
    mode_error = NULL;

    key();
    tx_heartbeat(2);
    g_now_us += HEARTBEAT_US;
    watchdog_tick();
    assert(off_writes == 1 && tx_get_state().phase == TX_KEYED);
    assert(tx_get_state().stopping && tx_get_state().off_attempts == 1 && tx_get_state().off_error == ESP_OK);
    assert(!tx_begin_usb_recovery()); // TX0 sent, RX not confirmed
    tx_unkey("duplicate");
    assert(off_writes == 1);
    confirm_rx();
    assert(tx_get_state().reason[0] == 'H');
    assert(!tx_get_state().stopping && tx_get_state().off_attempts == 1);
    assert(on_writes == 1);

    key();
    cat_available = false;
    watchdog_tick();
    assert(!tx_begin_usb_recovery()); // TX0 cannot reach the radio
    assert(tx_get_state().off_error == ESP_ERR_INVALID_STATE);
    assert(off_writes == 1);
    cat_available = true;
    assert(!tx_begin_usb_recovery()); // TX0 can now be sent
    assert(off_writes == 2 && tx_get_state().off_attempts >= 2 && tx_get_state().off_error == ESP_OK);
    confirm_rx();
    assert(tx_begin_usb_recovery());
    tx_resume();

    key();
    for (unsigned i = 0; i < 10; ++i) {
        g_now_us += 1000000;
        tx_heartbeat(1);
        watchdog_tick();
    }
    assert(off_writes == 3);
    confirm_rx();

    key();
    tx_release(2, "Other client");
    assert(off_writes == 3);
    tx_release(1, "Client disconnected");
    assert(off_writes == 4);
    confirm_rx();

    key();
    usb_available = false;
    watchdog_tick();
    assert(off_writes == 5);
    confirm_rx();
    usb_available = true;

    key();
    fault = "High SWR";
    watchdog_tick();
    assert(off_writes == 6);
    assert(!tx_quiesce("Reboot"));
    confirm_rx();
    assert(tx_quiesce("Reboot"));
    assert(!tx_acquire(1));
    tx_resume();
    key();
    assert(!tx_begin_usb_recovery());
    assert(off_writes == 7);
    assert(tx_get_state().phase == TX_KEYED);
    confirm_rx();
    assert(tx_begin_usb_recovery());
    tx_resume();
    key();
    fault = NULL;
    mode_error = "Set SUB mode to USB, LSB, DATA-USB or DATA-LSB";
    watchdog_tick();
    assert(off_writes == 8 && tx_get_state().reason == mode_error);
    confirm_rx();
    mode_error = NULL;
    rx_us = g_now_us;
    assert(tx_acquire(1));
    output_available = false;
    assert(!tx_key_tone(1, TX_TONE_SINGLE, -24));
    assert(on_writes == 8 && audio_request == TX_TONE_OFF);
    output_available = true;
    output_idle = false;
    assert(tx_acquire(1));
    assert(!tx_key_tone(1, TX_TONE_SINGLE, -24));
    assert(on_writes == 8 && audio_request == TX_TONE_OFF);
    output_idle = true;
    assert(tx_acquire(1));
    assert(tx_key_tone(1, TX_TONE_SINGLE, -24));
    assert(audio_request == TX_TONE_SINGLE && preparations == 1 && !prepare_shared);
    po_raw = 10;
    swr_raw = 126;
    g_now_us += 100000;
    watchdog_tick();
    assert(audio_request == TX_TONE_OFF && off_writes == 9);
    assert(tx_get_state().stop_swr == 300);
    assert(cleanups == 0);
    confirm_rx();
    assert(cleanups == 1);
    meters_missing = true;
    warnings[0] = '\0';
    key();
    g_now_us += 700000;
    watchdog_tick();
    assert(!s_stopping && off_writes == 9 && stale_diagnostics_reads == 0);
    g_now_us += 500000;
    watchdog_tick();
    assert(s_stopping && off_writes == 10 && stale_diagnostics_reads == 1);
    assert(strcmp(tx_get_state().reason, "SWR or power readings unavailable") == 0);
    assert(strstr(warnings, "cat_write.meters calls=6 timeouts=1 max_us=10000"));
    assert(strstr(warnings, "po_us=0 age_us=-1") && strstr(warnings, "swr_us=0 age_us=-1"));
    assert(strstr(warnings, "swr_po_us=0") && strstr(warnings, "RM5;"));
    confirm_rx();
    assert(stale_diagnostics_reads == 1);
    radio_model = CAT_RIG_FTX1;
    meters_missing = false;
    const unsigned on_before = on_writes;
    rx_us = g_now_us;
    prepare_fails = true;
    assert(tx_acquire(1));
    assert(!tx_key_tone(1, TX_TONE_SINGLE, -24));
    assert(preparations == 2 && prepare_shared && on_writes == on_before && cleanups == 1);
    prepare_fails = false;
    prepare_changes_mode = true;
    assert(tx_acquire(1));
    assert(!tx_key_tone(1, TX_TONE_SINGLE, -24));
    assert(preparations == 3 && on_writes == on_before && cleanups == 2);
    mode_error = NULL;
    prepare_changes_mode = false;
    assert(tx_acquire(1));
    assert(tx_key_tone(1, TX_TONE_SINGLE, -24));
    assert(preparations == 4 && prepare_shared && on_writes == on_before + 1);
    tx_release(1, "Released");
    assert(audio_request == TX_TONE_OFF && cleanups == 2);
    confirm_rx();
    assert(cleanups == 3);
    // Safety unkey should preserve the stop reason and reject re-key before cleanup.
    const unsigned off_before = off_writes;
    radio_on = false;
    radio_model = CAT_RIG_FTDX10;
    rx_us = ++g_now_us;
    assert(tx_acquire(1) && tx_key_tone(1, TX_TONE_SINGLE, -24));
    defer_audio_cleanup = true;
    fault = "High SWR";
    watchdog_tick();
    assert(tx_get_state().stopping && audio_request == TX_TONE_OFF);
    assert(off_writes == off_before + 1 && !cleanup_pending);
    radio_on = false;
    rx_us = ++g_now_us;
    watchdog_tick();
    assert(cleanup_pending && tx_get_state().stopping && !tx_get_state().on_air && tx_get_state().rx_confirmed);
    assert(!tx_acquire(1) && !tx_acquire(2));
    assert(strcmp(tx_get_state().reason, "High SWR") == 0);
    g_now_us += CONFIRM_US + 1;
    watchdog_tick();
    assert(off_writes == off_before + 1);
    output_idle = true;
    cleanup_pending = false;
    watchdog_tick();
    assert(tx_get_state().phase == TX_IDLE && !tx_get_state().stopping);
    fault = NULL;
    assert(tx_acquire(1) && tx_key_tone(1, TX_TONE_SINGLE, -24));
    defer_audio_cleanup = false;
    tx_release(1, "Released");
    confirm_rx();
    assert(tx_acquire(1) && tx_key_tone(1, TX_TONE_VOICE, -24));
    uint8_t frame[TX_VOICE_FRAME_BYTES] = {3, 2};
    assert(tx_submit_voice(2, frame, sizeof(frame)) && voice_frames == 0);
    assert(!tx_submit_voice(1, frame, sizeof(frame) - 1) && voice_frames == 0);
    uint8_t packet[TX_VOICE_PACKET_BYTES] = {3, 2};
    assert(!tx_submit_voice(1, packet, sizeof(packet) - 1));
    assert(tx_submit_voice(1, packet, sizeof(packet)) && voice_frames == 2);
    g_now_us += HEARTBEAT_US - 1;
    assert(tx_submit_voice(1, frame, sizeof(frame)) && voice_frames == 3);
    ++g_now_us;
    assert(tx_submit_voice(1, frame, sizeof(frame)) && voice_frames == 3);
    watchdog_tick();
    assert(tx_get_state().stopping && strcmp(tx_get_state().reason, "Heartbeat lost") == 0);
    confirm_rx();
    puts("all TX state tests passed");
}
