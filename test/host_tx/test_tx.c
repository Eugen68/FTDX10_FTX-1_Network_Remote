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
static tx_tone_t audio_request;
static unsigned po_raw, swr_raw;
static bool meters_missing;
static unsigned stale_diagnostics_reads;
static unsigned on_writes, off_writes;
static const char *fault;
static const char *mode_error;

const char *cat_ptt_mode_error(void) { return mode_error; }

bool app_config_tx_enabled(void) { return enabled; }
void app_config_get(app_config_t *cfg) { *cfg = (app_config_t){.tx_max_seconds = 10, .swr_max = 30}; }
bool usb_audio_get_format(usb_audio_format_t *fmt) { return usb_available; }
bool usb_audio_tx_available(void) { return output_available; }
bool usb_audio_tx_idle(void) { return output_idle; }
void usb_audio_tx_request(tx_tone_t tone, int dbfs) { audio_request = tone; }
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
    assert(s_stopping && off_writes == 10);
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
    assert(audio_request == TX_TONE_SINGLE);
    po_raw = 10;
    swr_raw = 126;
    g_now_us += 100000;
    watchdog_tick();
    assert(audio_request == TX_TONE_OFF && off_writes == 9);
    assert(tx_get_state().stop_swr == 300);
    confirm_rx();
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
    puts("all TX state tests passed");
}
