/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdio.h>
#include "tx.c"

int64_t g_now_us = 1000000;
static int64_t rx_us;
static bool enabled = true;
static bool radio_on;
static bool cat_available = true;
static bool usb_available = true;
static unsigned on_writes, off_writes;
static const char *fault;
static const char *mode_error;

const char *cat_ptt_mode_error(void) { return mode_error; }

bool app_config_tx_enabled(void) { return enabled; }
void app_config_get(app_config_t *cfg) { *cfg = (app_config_t){.tx_max_seconds = 10}; }
bool usb_audio_get_format(usb_audio_format_t *fmt) { return usb_available; }
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
    assert(!tx_begin_usb_recovery()); // TX0 sent, RX not confirmed
    tx_unkey("duplicate");
    assert(off_writes == 1);
    confirm_rx();
    assert(tx_get_state().reason[0] == 'H');
    assert(on_writes == 1);

    key();
    cat_available = false;
    watchdog_tick();
    assert(!tx_begin_usb_recovery()); // TX0 cannot reach the radio
    assert(off_writes == 1);
    cat_available = true;
    assert(!tx_begin_usb_recovery()); // TX0 can now be sent
    assert(off_writes == 2);
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
    puts("all TX state tests passed");
}
