/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <inttypes.h>
#include "tx.h"
#include "app_config.h"
#include "cat.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_audio.h"
#include "tx_swr.h"
#include "tx_voice.h"

#define HEARTBEAT_US 1500000LL
#define CONFIRM_US 1500000LL

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_watchdog_task;
static esp_timer_handle_t s_timer;
static tx_state_t s_state = {.reason = ""};
static int64_t s_heartbeat, s_keyed_at, s_deadline, s_off_sent;
static bool s_stopping, s_inhibited, s_waiting_for_audio_cleanup;
static tx_swr_guard_t s_swr_guard;
static unsigned s_swr_max;
static tx_tone_t s_tone;
// The audio task reads this without taking the TX lock while holding its USB lock.
static int64_t s_ft8_feed_at; // published by the release/acquire USB request

int64_t tx_ft8_feed_time(void)
{
    return s_ft8_feed_at;
}

static void finish_stop(void)
{
    s_state.phase = TX_IDLE;
    s_state.owner = 0;
    s_stopping = false;
    s_waiting_for_audio_cleanup = false;
    s_state.stopping = false;
    s_off_sent = 0;
}

static void stop_locked(const char *reason)
{
    if (s_state.phase == TX_IDLE) {
        return;
    }
    if (!s_stopping) {
        s_state.reason = reason;
        ESP_LOGI("tx", "PTT stop requested: %s", reason);
    }
    if (s_state.phase == TX_ARMED) {
        finish_stop();
        return;
    }

    s_stopping = true;
    s_state.stopping = true;
    if (s_waiting_for_audio_cleanup) {
        if (!usb_audio_tx_cleanup_pending()) {
            finish_stop();
        }
        return;
    }

    usb_audio_tx_request(TX_TONE_OFF, 0);
    if (s_tone == TX_TONE_VOICE) tx_voice_stop();
    if (s_off_sent && cat_radio_rx_after(s_off_sent)) {
        s_state.rx_confirmed = true;
        ESP_LOGI("tx", "RX confirmed after %u TX0 attempt(s)", s_state.off_attempts);
        if (s_tone != TX_TONE_OFF) {
            usb_audio_tx_finished();
            s_waiting_for_audio_cleanup = usb_audio_tx_cleanup_pending();
        }
        if (!s_waiting_for_audio_cleanup) {
            finish_stop();
        }
    } else if (!s_off_sent || esp_timer_get_time() - s_off_sent >= CONFIRM_US) {
        const esp_err_t previous = s_state.off_error;
        s_state.off_attempts++;
        s_state.off_error = cat_ptt(false);
        if (s_state.off_error == ESP_OK) {
            s_off_sent = esp_timer_get_time();
            ESP_LOGI("tx", "TX0 written; waiting for fresh RX confirmation");
        } else if (s_state.off_attempts == 1 || previous != s_state.off_error) {
            ESP_LOGW("tx", "TX0 write failed: %s", esp_err_to_name(s_state.off_error));
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
    return tx_key_tone(owner, TX_TONE_OFF, TX_TONE_DEFAULT_DBFS);
}

static const char *ft8_mode_error(const cat_state_t *radio, bool starting)
{
    if (radio->rig != CAT_RIG_FTX1 && radio->rig != CAT_RIG_FTDX10) return "Radio not supported for FT8";
    const char mode = radio->tx_vfo == CAT_TX_VFO_SUB ? radio->mode_sub_raw : radio->mode_main_raw;
    if (mode == '2') return cat_voice_mode_error(starting);
    return mode == 'C' ? NULL : "Select USB or DATA-USB for FT8 TX";
}

static const char *current_ft8_mode_error(bool starting)
{
    cat_state_t radio;
    cat_get_state(&radio);
    return ft8_mode_error(&radio, starting);
}

static bool key_audio(uint32_t owner, tx_tone_t tone, int dbfs, int64_t feed_at)
{
    if (tone < TX_TONE_OFF || tone > TX_TONE_FT8 || dbfs < TX_TONE_MIN_DBFS || dbfs > TX_TONE_MAX_DBFS) {
        tx_release(owner, "Invalid TX audio settings");
        return false;
    }
    app_config_t cfg;
    app_config_get(&cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool keyed = false;
    const char *refusal = "PTT unavailable";
    if (s_state.phase == TX_ARMED && s_state.owner == owner && !s_inhibited &&
        usb_audio_get_format(NULL) && cat_radio_rx_after(0)) {
        cat_state_t radio;
        cat_get_state(&radio);
        const char *mode_error = cat_ptt_mode_error(0);
        if (!mode_error && tone == TX_TONE_VOICE) mode_error = cat_voice_mode_error(true);
        if (!mode_error && tone == TX_TONE_FT8) {
            mode_error = cfg.tx_max_seconds < 15 ? "FT8 needs a TX limit of at least 15 seconds"
                                                : ft8_mode_error(&radio, true);
        }
        if (tone != TX_TONE_OFF && !usb_audio_tx_available()) {
            refusal = "USB TX audio unavailable";
        } else if (!usb_audio_tx_idle()) {
            refusal = usb_audio_tx_cleanup_pending() ? "Waiting for USB TX audio to stop"
                                                      : "USB TX audio not ready";
        } else if (mode_error) {
            refusal = mode_error;
        } else if (tone == TX_TONE_VOICE && !tx_voice_start()) {
            refusal = "USB voice buffer unavailable";
        } else if (tone != TX_TONE_OFF && usb_audio_tx_prepare(radio.rig == CAT_RIG_FTX1) != ESP_OK) {
            refusal = "USB TX audio start failed";
        } else if (tone == TX_TONE_FT8 && esp_timer_get_time() >= feed_at) {
            usb_audio_tx_finished();
            refusal = TX_FT8_SLOT_MISSED;
        } else if (tone != TX_TONE_OFF &&
                   (esp_timer_get_time() - s_heartbeat >= HEARTBEAT_US ||
                    !usb_audio_tx_available() || !usb_audio_get_format(NULL) ||
                    !cat_radio_rx_after(0) || cat_ptt_mode_error(0) ||
                    (tone == TX_TONE_VOICE && cat_voice_mode_error(true)) ||
                    (tone == TX_TONE_FT8 && current_ft8_mode_error(true)))) {
            usb_audio_tx_finished();
            refusal = "PTT conditions changed during USB audio setup";
        } else {
            s_keyed_at = esp_timer_get_time();
            s_deadline = s_keyed_at + (int64_t)cfg.tx_max_seconds * 1000000;
            s_state.phase = TX_KEYED;  // even a failed write may have reached the radio
            s_tone = tone;
            s_swr_max = cfg.swr_max * 10;
            s_swr_guard = (tx_swr_guard_t){.rig = radio.rig};
            keyed = cat_ptt(true) == ESP_OK;
            if (keyed && tone != TX_TONE_OFF) {
                if (tone == TX_TONE_FT8) s_ft8_feed_at = feed_at;
                usb_audio_tx_request(tone, dbfs);
            }
        }
    }
    if (!keyed && s_state.owner == owner) {
        stop_locked(refusal);
    }
    xSemaphoreGive(s_lock);
    return keyed;
}

bool tx_key_tone(uint32_t owner, tx_tone_t tone, int dbfs)
{
    if (tone == TX_TONE_FT8) return false;
    return key_audio(owner, tone, dbfs, 0);
}

bool tx_key_ft8(uint32_t owner, int64_t feed_at)
{
    return key_audio(owner, TX_TONE_FT8, TX_TONE_DEFAULT_DBFS, feed_at);
}

bool tx_submit_voice(uint32_t owner, const uint8_t *frame, size_t length)
{
    if (!tx_voice_packet_size_valid(length) || frame[0] != 3 || frame[1] != TX_VOICE_RATE / 8000) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (owner && owner == s_state.owner && s_state.phase == TX_KEYED && !s_stopping &&
        s_tone == TX_TONE_VOICE && esp_timer_get_time() - s_heartbeat < HEARTBEAT_US) {
        for (size_t offset = 2; offset < length; offset += 2 * TX_VOICE_SAMPLES) {
            tx_voice_push(frame + offset);
        }
    }
    xSemaphoreGive(s_lock);
    return true;
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

static void log_stale_meters(const cat_tx_meters_t *meters, int64_t keyed_at, int64_t now)
{
    const struct {
        const char *name;
        int64_t stamp;
        unsigned raw;
    } samples[] = {
        {"po_us", meters->po_us, meters->po},
        {"swr_us", meters->swr_us, meters->swr},
        {"swr_po_us", meters->swr_po_us, meters->swr_po},
    };
    const cat_write_timing_t writes = cat_get_write_stats().meters;
    ESP_LOGW("tx", "STALE now_us=%" PRId64 " keyed_at_us=%" PRId64
             " cat_write.meters calls=%u timeouts=%u max_us=%u",
             now, keyed_at, writes.calls, writes.timeouts, writes.max_us);
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const int64_t stamp = samples[i].stamp;
        ESP_LOGW("tx", "STALE %s=%" PRId64 " age_us=%" PRId64 " since_keyed_us=%" PRId64 " raw=%u",
                 samples[i].name, stamp, stamp ? now - stamp : -1,
                 stamp ? stamp - keyed_at : 0, samples[i].raw);
    }
    cat_log_entry_t entries[12];
    size_t count = cat_log_recent_tx_meters((uint32_t)(now / 1000), entries,
                                            sizeof(entries) / sizeof(entries[0]));
    if (!count) {
        ESP_LOGW("tx", "STALE CAT log: no recent RM5/RM6 entries");
    }
    while (count > 0) {
        const cat_log_entry_t *entry = &entries[--count];
        ESP_LOGW("tx", "STALE CAT seq=%" PRIu32 " ms=%" PRIu32 " %c kind=%u %s",
                 entry->seq, entry->ms, entry->dir, entry->kind, entry->text);
    }
}

static void watchdog_tick(void)
{
    cat_tx_meters_t stale_meters;
    int64_t stale_at = 0, stale_keyed_at = 0;
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
            } else if (!usb_audio_get_format(NULL) || (s_tone != TX_TONE_OFF && !usb_audio_tx_available())) {
                stop_locked("USB audio lost");
            } else {
                const char *reason = cat_ptt_fault(s_keyed_at, CONFIRM_US);
                if (!reason) {
                    reason = cat_ptt_mode_error(s_keyed_at);
                }
                if (!reason && s_tone == TX_TONE_VOICE) reason = cat_voice_mode_error(false);
                if (!reason && s_tone == TX_TONE_FT8) reason = current_ft8_mode_error(false);
                if (!reason) {
                    const cat_tx_meters_t meters = cat_get_tx_meters();
                    const int64_t meter_now = esp_timer_get_time();
                    const tx_swr_result_t result = tx_swr_check(&s_swr_guard, &meters, s_keyed_at, meter_now, s_swr_max);
                    if (result == TX_SWR_STALE) {
                        stale_meters = meters;
                        stale_at = meter_now;
                        stale_keyed_at = s_keyed_at;
                        reason = "SWR or power readings unavailable";
                    } else if (result == TX_SWR_LIMIT || result == TX_SWR_RISE) {
                        s_state.stop_swr = tx_swr_value(meters.rig, meters.swr);
                        reason = result == TX_SWR_RISE ? "rise" : "limit";
                    }
                }
                if (reason) {
                    stop_locked(reason);
                }
            }
        }
    }
    xSemaphoreGive(s_lock);
    // Send TX0 before collecting or printing diagnostics.
    if (stale_at) {
        log_stale_meters(&stale_meters, stale_keyed_at, stale_at);
    }
}

tx_state_t tx_get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_state_t result = s_state;
    const int64_t now = esp_timer_get_time();
    const int64_t remaining = s_deadline - now;
    result.left_s = s_state.phase == TX_KEYED && !s_stopping && remaining > 0
                    ? (unsigned)((remaining + 999999) / 1000000) : 0;
    result.on_air = s_state.phase == TX_KEYED && cat_radio_tx();
    const cat_tx_meters_t meters = cat_get_tx_meters();
    if (s_keyed_at > 0) {
        const int64_t max_age_us = tx_meter_max_age_us(s_swr_guard.rig);
        result.po_valid = meters.po_us >= s_keyed_at && now - meters.po_us <= max_age_us;
        result.swr_valid = result.po_valid && meters.po >= TX_PO_MIN_RAW &&
                           meters.swr_po >= TX_PO_MIN_RAW && meters.swr_po_us >= s_keyed_at &&
                           now - meters.swr_us <= max_age_us;
        result.swr = result.swr_valid ? tx_swr_value(meters.rig, meters.swr) : 0;
        result.swr_raw = meters.swr_us >= s_keyed_at ? meters.swr : 0;
        result.po_raw = meters.po_us >= s_keyed_at ? meters.po : 0;
    }
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
    if (!s_lock || xTaskCreatePinnedToCore(watchdog_task, "tx_watchdog", 4096, NULL, 8, &s_watchdog_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = {.callback = watchdog_alarm, .name = "tx_watchdog"};
    esp_err_t err = esp_timer_create(&args, &s_timer);
    return err == ESP_OK ? esp_timer_start_periodic(s_timer, 100000) : err;
}
