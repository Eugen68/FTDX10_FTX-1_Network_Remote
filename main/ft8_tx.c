/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "ft8_tx.h"

#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cat.h"
#include "tx.h"
#include "usb_audio.h"

#define HEARTBEAT_US 1500000LL
#define NTP_MAX_AGE_US 1800000000LL
#define BROWSER_MAX_AGE_US 15000000LL
#define MAX_CLOCK_CHANGE_US 80000LL
#define PREPARE_LEAD_US 300000LL
#define PREPARE_LATE_US 50000LL
#define SLOT_LIMIT 3

typedef struct {
    int64_t offset_us, sampled_us;
    unsigned rtt_ms;
    uint32_t owner;
} ft8_clock_t;

typedef struct {
    uint32_t owner;
    bool odd, ready, started, finishing;
    unsigned hz, remaining;
    int dbfs;
    int64_t heartbeat, slot, offset_us, end_us;
    uint8_t tones[FT8_TX_SYMBOLS];
    const char *reason;
} ft8_job_t;

static SemaphoreHandle_t s_lock;
static ft8_clock_t s_ntp, s_browser;
static ft8_job_t s_job = {.reason = ""};
static int16_t *s_pcm;
static bool s_rendering;

static ft8_clock_t clock_locked(uint32_t owner, int64_t now)
{
    if (s_ntp.sampled_us && now - s_ntp.sampled_us < NTP_MAX_AGE_US) return s_ntp;
    if (s_browser.owner == owner && s_browser.sampled_us &&
        now - s_browser.sampled_us < BROWSER_MAX_AGE_US) return s_browser;
    return (ft8_clock_t){0};
}

static void ntp_synced(struct timeval *utc)
{
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ntp = (ft8_clock_t){.offset_us = (int64_t)utc->tv_sec * 1000000 + utc->tv_usec - now,
                         .sampled_us = now};
    xSemaphoreGive(s_lock);
}

void ft8_tx_start_clock(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_FT8_NTP_SERVER);
    config.sync_cb = ntp_synced;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) ESP_LOGW("ft8_tx", "SNTP startup: %s", esp_err_to_name(err));
}

void ft8_tx_browser_clock(uint32_t owner, int64_t offset_ms, unsigned rtt_ms)
{
    if (!owner || rtt_ms > 160 || offset_ms < 1700000000000LL || offset_ms > 4102444800000LL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_job.owner || s_job.owner == owner) {
        s_browser = (ft8_clock_t){.offset_us = offset_ms * 1000, .sampled_us = esp_timer_get_time(),
                                  .rtt_ms = rtt_ms, .owner = owner};
    }
    xSemaphoreGive(s_lock);
}

static void stop_locked(const char *reason)
{
    tx_release(s_job.owner, reason);
    s_job = (ft8_job_t){.reason = reason};
}

void ft8_tx_halt(uint32_t owner, const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (owner && s_job.owner == owner) stop_locked(reason);
    if (s_browser.owner == owner) s_browser = (ft8_clock_t){0};
    xSemaphoreGive(s_lock);
}

void ft8_tx_heartbeat(uint32_t owner)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    if (owner && s_job.owner == owner && now - s_job.heartbeat < HEARTBEAT_US) {
        s_job.heartbeat = now;
        tx_heartbeat(owner);
    }
    xSemaphoreGive(s_lock);
}

const char *ft8_tx_arm(uint32_t owner, const char *message, unsigned hz, bool odd, int dbfs)
{
    uint8_t tones[FT8_TX_SYMBOLS];
    if (hz < 200 || hz > 3000 || dbfs < TX_TONE_MIN_DBFS || dbfs > TX_TONE_MAX_DBFS ||
        !ft8_wave_pack(message, tones)) return "Invalid FT8 message or audio settings";

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *error = "";
    const int64_t now = esp_timer_get_time();
    if (!s_pcm) error = "FT8 PSRAM unavailable";
    else if (s_job.owner || s_rendering) error = "FT8 busy";
    else if (!clock_locked(owner, now).sampled_us) error = "Waiting for a fresh FT8 clock";
    else if (!tx_acquire(owner)) error = "TX busy or unavailable";
    else {
        s_job = (ft8_job_t){.owner = owner, .odd = odd, .hz = hz, .dbfs = dbfs,
                            .remaining = SLOT_LIMIT, .heartbeat = now, .reason = "Preparing FT8"};
        for (unsigned i = 0; i < FT8_TX_SYMBOLS; ++i) s_job.tones[i] = tones[i];
    }
    xSemaphoreGive(s_lock);
    return error;
}

static unsigned audio_lead_ms(void)
{
    cat_state_t radio;
    cat_get_state(&radio);
    return radio.rig == CAT_RIG_FTX1 ? CONFIG_FT8_FTX1_AUDIO_LEAD_MS : CONFIG_FT8_FTDX10_AUDIO_LEAD_MS;
}

ft8_tx_status_t ft8_tx_status(uint32_t owner)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const ft8_clock_t clock = clock_locked(owner ? owner : s_browser.owner, now);
    const ft8_tx_status_t status = {
        .armed = s_job.owner != 0, .owned = owner && owner == s_job.owner,
        .remaining = s_job.remaining, .reason = s_job.reason,
        .clock = clock.sampled_us ? (clock.owner ? "browser" : "sntp") : "none",
        .age_ms = clock.sampled_us ? (now - clock.sampled_us) / 1000 : -1,
        .offset_ms = clock.offset_us / 1000, .rtt_ms = clock.rtt_ms, .lead_ms = audio_lead_ms(),
    };
    xSemaphoreGive(s_lock);
    return status;
}

// The TX state is IDLE only after the USB playback task has stopped, so the
// scheduler cannot begin rendering another frame while this buffer is in use.
void ft8_tx_copy(int16_t *pcm, unsigned offset, unsigned count, unsigned channels)
{
    for (unsigned i = 0; i < count; ++i) {
        int16_t sample = offset + i < FT8_TX_SAMPLES ? s_pcm[offset + i] : 0;
        for (unsigned ch = 0; ch < channels; ++ch) *pcm++ = sample;
    }
}

static bool yield_render(void)
{
    vTaskDelay(1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool alive = s_job.owner && esp_timer_get_time() - s_job.heartbeat < HEARTBEAT_US;
    xSemaphoreGive(s_lock);
    return alive;
}

static void ft8_tick(void)
{
    if (!s_job.owner) return;
    const int64_t now = esp_timer_get_time();
    const ft8_clock_t clock = clock_locked(s_job.owner, now);
    const tx_state_t state = tx_get_state();
    if (now - s_job.heartbeat >= HEARTBEAT_US) {
        stop_locked("FT8 heartbeat lost");
    } else if (!clock.sampled_us) {
        stop_locked("FT8 clock expired: check SNTP or browser RTT");
    } else if (s_job.slot && llabs(clock.offset_us - s_job.offset_us) > MAX_CLOCK_CHANGE_US) {
        stop_locked("FT8 clock offset changed: re-arm for a new slot");
    } else if (s_job.finishing) {
        if (state.phase != TX_IDLE) return;
        if (!s_job.remaining) stop_locked("FT8 transmission limit reached");
        else if (!tx_acquire(s_job.owner)) stop_locked("TX busy or unavailable");
        else {
            s_job.finishing = s_job.started = false;
            s_job.slot = 0;
        }
    } else if (state.owner != s_job.owner || state.stopping) {
        if (s_job.started && strcmp(state.reason, TX_FT8_SLOT_MISSED) == 0) {
            // No FT8 samples were submitted. Wait for RX, then schedule a later slot.
            s_job.finishing = true;
        } else {
            stop_locked(state.reason[0] ? state.reason : "FT8 TX stopped");
        }
    } else if (s_job.started) {
        if (now >= s_job.end_us) {
            if (usb_audio_ft8_complete()) {
                tx_release(s_job.owner, "FT8 frame complete");
                s_job.finishing = true;
            } else {
                stop_locked("FT8 playback missed its deadline");
            }
        }
    } else if (s_job.ready) {
        if (!s_job.slot) {
            s_job.offset_us = clock.offset_us;
            s_job.slot = ft8_next_slot(now + clock.offset_us, s_job.odd, PREPARE_LEAD_US) - clock.offset_us;
            s_job.reason = "Waiting for FT8 slot";
        }
        const int64_t prepare = s_job.slot - PREPARE_LEAD_US;
        if (now < prepare) return;
        if (now > prepare + PREPARE_LATE_US) {
            s_job.slot = 0;
            return;
        }
        const int64_t audio_at = s_job.slot + FT8_TX_AUDIO_US;
        const int64_t feed_at = audio_at - audio_lead_ms() * 1000LL;
        --s_job.remaining;
        if (!tx_key_ft8(s_job.owner, feed_at)) {
            const tx_state_t refused = tx_get_state();
            if (strcmp(refused.reason, TX_FT8_SLOT_MISSED) == 0) {
                s_job.finishing = true;
            } else {
                stop_locked(refused.reason[0] ? refused.reason : "FT8 PTT refused");
            }
        } else {
            s_job.started = true;
            s_job.end_us = audio_at + FT8_TX_FRAME_US + 30000;
            s_job.reason = "FT8 transmitting";
        }
    }
}

static void ft8_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_job.owner && !s_job.ready) {
            const ft8_job_t job = s_job;
            s_rendering = true;
            xSemaphoreGive(s_lock);
            const int64_t start = esp_timer_get_time();
            const bool ready = ft8_wave_render(s_pcm, job.tones, job.hz, job.dbfs, yield_render);
            if (ready) {
                ESP_LOGI("ft8_tx", "Prepared %u PCM bytes in %lld ms", (unsigned)(FT8_TX_SAMPLES * sizeof(*s_pcm)),
                         (long long)((esp_timer_get_time() - start) / 1000));
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_rendering = false;
            if (s_job.owner == job.owner) {
                if (ready) s_job.ready = true;
                else stop_locked("FT8 frame preparation cancelled");
            }
        }
        ft8_tick();
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t ft8_tx_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_pcm = heap_caps_malloc(FT8_TX_SAMPLES * sizeof(*s_pcm), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (xTaskCreatePinnedToCore(ft8_task, "ft8_tx", 6144, NULL, 3, NULL, 0) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
