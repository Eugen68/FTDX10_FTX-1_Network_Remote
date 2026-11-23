/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "tx.h"
#include "app_config.h"
#include "cat.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_audio.h"

#define HEARTBEAT_US 1500000LL
#define CONFIRM_US 1500000LL

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_watchdog_task;
static esp_timer_handle_t s_timer;
static tx_state_t s_state = {.reason = ""};
static int64_t s_heartbeat, s_keyed_at, s_deadline, s_off_sent;
static bool s_stopping, s_inhibited;

static void finish_stop(void)
{
    s_state.phase = TX_IDLE;
    s_state.owner = 0;
    s_stopping = false;
    s_off_sent = 0;
}

static void stop_locked(const char *reason)
{
    if (s_state.phase == TX_IDLE) {
        return;
    }
    if (!s_stopping) {
        s_state.reason = reason;
    }
    if (s_state.phase == TX_ARMED) {
        finish_stop();
        return;
    }

    s_stopping = true;
    if (s_off_sent && cat_radio_rx_after(s_off_sent)) {
        finish_stop();
    } else if (!s_off_sent || esp_timer_get_time() - s_off_sent >= CONFIRM_US) {
        if (cat_ptt(false) == ESP_OK) {
            s_off_sent = esp_timer_get_time();
        }
    }
}

bool tx_acquire(uint32_t owner)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool granted = owner && !s_inhibited && app_config_tx_enabled() && s_state.phase == TX_IDLE;
    if (granted) {
        s_state = (tx_state_t){.phase = TX_ARMED, .owner = owner, .reason = ""};
        s_heartbeat = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
    return granted;
}

bool tx_key(uint32_t owner)
{
    app_config_t cfg;
    app_config_get(&cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool keyed = false;
    const char *refusal = "PTT unavailable";
    if (s_state.phase == TX_ARMED && s_state.owner == owner && !s_inhibited &&
        usb_audio_get_format(NULL) && cat_radio_rx_after(0)) {
        const char *mode_error = cat_ptt_mode_error();
        if (mode_error) {
            refusal = mode_error;
        } else {
            s_keyed_at = esp_timer_get_time();
            s_deadline = s_keyed_at + (int64_t)cfg.tx_max_seconds * 1000000;
            s_state.phase = TX_KEYED;  // even a failed write may have reached the radio
            keyed = cat_ptt(true) == ESP_OK;
        }
    }
    if (!keyed && s_state.owner == owner) {
        stop_locked(refusal);
    }
    xSemaphoreGive(s_lock);
    return keyed;
}

void tx_heartbeat(uint32_t owner)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    if (owner && owner == s_state.owner && !s_stopping && now - s_heartbeat < HEARTBEAT_US) {
        s_heartbeat = now;
    }
    xSemaphoreGive(s_lock);
}

void tx_release(uint32_t owner, const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (owner && owner == s_state.owner) {
        stop_locked(reason);
    }
    xSemaphoreGive(s_lock);
}

void tx_unkey(const char *reason)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        stop_locked(reason);
        xSemaphoreGive(s_lock);
    }
}

static void watchdog_tick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.phase != TX_IDLE) {
        const int64_t now = esp_timer_get_time();
        if (s_stopping) {
            stop_locked(s_state.reason);
        } else if (now - s_heartbeat >= HEARTBEAT_US) {
            stop_locked("Heartbeat lost");
        } else if (s_state.phase == TX_KEYED) {
            if (now >= s_deadline) {
                stop_locked("TX time limit");
            } else if (!usb_audio_get_format(NULL)) {
                stop_locked("USB audio lost");
            } else {
                const char *reason = cat_ptt_fault(s_keyed_at, CONFIRM_US);
                if (!reason) {
                    reason = cat_ptt_mode_error();
                }
                if (reason) {
                    stop_locked(reason);
                }
            }
        }
    }
    xSemaphoreGive(s_lock);
}

tx_state_t tx_get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_state_t result = s_state;
    const int64_t remaining = s_deadline - esp_timer_get_time();
    result.left_s = s_state.phase == TX_KEYED && !s_stopping && remaining > 0
                    ? (unsigned)((remaining + 999999) / 1000000) : 0;
    result.on_air = s_state.phase == TX_KEYED && cat_radio_tx();
    xSemaphoreGive(s_lock);
    return result;
}

bool tx_begin_usb_recovery(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.phase != TX_IDLE) {
        stop_locked("USB recovery");
    }
    const bool allowed = !s_inhibited && s_state.phase == TX_IDLE && !cat_radio_tx();
    if (allowed) {
        s_inhibited = true;
    }
    xSemaphoreGive(s_lock);
    return allowed;
}

bool tx_quiesce(const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_locked(reason);
    const bool allowed = !s_inhibited && s_state.phase == TX_IDLE;
    if (allowed) {
        s_inhibited = true;
    }
    xSemaphoreGive(s_lock);
    return allowed;
}

void tx_resume(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_inhibited = false;
    xSemaphoreGive(s_lock);
}

static void watchdog_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        watchdog_tick();
    }
}

static void watchdog_alarm(void *arg)
{
    xTaskNotifyGive(s_watchdog_task);
}

esp_err_t tx_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock || xTaskCreate(watchdog_task, "tx_watchdog", 4096, NULL, 8, &s_watchdog_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = {.callback = watchdog_alarm, .name = "tx_watchdog"};
    esp_err_t err = esp_timer_create(&args, &s_timer);
    return err == ESP_OK ? esp_timer_start_periodic(s_timer, 100000) : err;
}
