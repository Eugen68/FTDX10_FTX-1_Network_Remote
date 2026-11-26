/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_FT8_NTP_SERVER "pool.ntp.org"
#define CONFIG_FT8_FTX1_AUDIO_LEAD_MS 20
#define CONFIG_FT8_FTDX10_AUDIO_LEAD_MS 45
#include "ft8_tx.c"

int64_t g_now_us = 1000000;
static tx_state_t state = {.reason = ""};
static unsigned keyed;
static bool complete = true;
static bool miss;
static int64_t feed_at, feed_offset, setup_delay_us;
static cat_rig_t model = CAT_RIG_FTDX10;

void cat_get_state(cat_state_t *radio) { *radio = (cat_state_t){.rig = model}; }
bool tx_acquire(uint32_t owner)
{
    if (state.phase != TX_IDLE) return false;
    state = (tx_state_t){.phase = TX_ARMED, .owner = owner, .reason = ""};
    return true;
}
bool tx_key_ft8(uint32_t owner, int64_t feed, int64_t utc_offset)
{
    assert(state.owner == owner && state.phase == TX_ARMED);
    g_now_us += setup_delay_us;
    if (miss || g_now_us >= feed) {
        tx_release(owner, TX_FT8_SLOT_MISSED);
        return false;
    }
    feed_at = feed;
    feed_offset = utc_offset;
    state.phase = TX_KEYED;
    ++keyed;
    return true;
}
void tx_release(uint32_t owner, const char *reason)
{
    if (state.owner == owner) state = (tx_state_t){.reason = reason};
}
void tx_heartbeat(uint32_t owner) { assert(owner == 1); }
tx_state_t tx_get_state(void) { return state; }
bool usb_audio_ft8_complete(void) { return complete; }

static void advance_with_heartbeats(int64_t target)
{
    while (g_now_us + 250000 < target) {
        g_now_us += 250000;
        ft8_tx_heartbeat(1);
    }
    g_now_us = target;
    ft8_tx_heartbeat(1);
}

static int64_t kick(void)
{
    s_job.ready = true;
    ft8_tick();
    const int64_t slot = s_job.slot;
    assert(slot > g_now_us);
    advance_with_heartbeats(slot - (model == CAT_RIG_FTX1 ? FTX1_PREPARE_LEAD_US : PREPARE_LEAD_US));
    ft8_tick();
    return slot;
}

int main(void)
{
    assert(ft8_tx_init() == ESP_OK);
    const int64_t utc_ms = 1800000000000LL;
    char timestamp[FT8_UTC_TEXT_SIZE];
    ft8_format_utc(utc_ms * 1000 + 123456, timestamp);
    assert(strcmp(timestamp, "2027-01-15T08:00:00.123Z") == 0);
    assert(*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, false, -24));
    ft8_tx_browser_clock(2, utc_ms, 20);
    assert(*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, false, -24));
    ft8_tx_browser_clock(1, utc_ms, 180);
    assert(*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, false, -24));
    ft8_tx_browser_clock(1, utc_ms, 20);
    struct timeval ntp = {.tv_sec = 1800000001};
    ntp_synced(&ntp);
    assert(strcmp(ft8_tx_status(1).clock, "sntp") == 0);

    assert(!*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, false, -24));
    for (unsigned count = 0; count < SLOT_LIMIT; ++count) {
        const int64_t slot = kick();
        assert(state.phase == TX_KEYED && keyed == count + 1);
        assert(feed_at == slot + FT8_TX_AUDIO_US - 45000LL);
        assert((feed_at + feed_offset) % FT8_TX_SLOT_US == 455000);
        assert((slot + s_ntp.offset_us) % (2 * FT8_TX_SLOT_US) == 0);
        advance_with_heartbeats(s_job.end_us);
        ft8_tick();
        assert(state.phase == TX_IDLE && s_job.finishing);
        ft8_tick();
    }
    assert(!s_job.owner && strcmp(s_job.reason, "FT8 transmission limit reached") == 0);
    assert(!*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, true, -24));
    ft8_tx_halt(2, "Wrong owner");
    assert(s_job.owner == 1);
    g_now_us += HEARTBEAT_US;
    ft8_tick();
    assert(!s_job.owner && strcmp(s_job.reason, "FT8 heartbeat lost") == 0);

    assert(!*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, true, -24));
    miss = true;
    kick();
    assert(state.phase == TX_IDLE && s_job.finishing);
    ft8_tick();
    assert(s_job.remaining == 2 && s_job.slot == 0);
    miss = false;
    kick();
    assert(state.phase == TX_KEYED);
    complete = false;
    advance_with_heartbeats(s_job.end_us);
    ft8_tick();
    assert(!s_job.owner && strcmp(s_job.reason, "FT8 playback missed its deadline") == 0);

    assert(!*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, true, -24));
    kick();
    s_ntp.offset_us += MAX_CLOCK_CHANGE_US + 1;
    ft8_tick();
    assert(!s_job.owner && strcmp(s_job.reason, "FT8 clock offset changed: re-arm for a new slot") == 0);
    s_ntp = (ft8_clock_t){0};
    ft8_tx_browser_clock(1, utc_ms, 20);
    assert(!*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, false, -24));
    ft8_tx_browser_clock(1, utc_ms, 200);
    assert(ft8_tx_status(1).rtt_ms == 20);
    advance_with_heartbeats(g_now_us + BROWSER_MAX_AGE_US);
    ft8_tick();
    assert(!s_job.owner && strcmp(s_job.reason, "FT8 clock expired: check SNTP or browser RTT") == 0);
    ntp_synced(&ntp);
    model = CAT_RIG_FTX1;
    setup_delay_us = 800000;
    assert(!*ft8_tx_arm(1, "CQ K1ABC FN42", 1500, false, -24));
    const int64_t ftx1_slot = kick();
    assert(state.phase == TX_KEYED);
    assert(g_now_us == ftx1_slot - FTX1_PREPARE_LEAD_US + setup_delay_us);
    assert(feed_at == ftx1_slot + FT8_TX_AUDIO_US - 20000LL);
    assert((feed_at + feed_offset) % FT8_TX_SLOT_US == 480000);
    ft8_tx_halt(1, "Done");
    free(s_pcm);
    puts("FT8 clock selection, ownership, slot limit, late schedule and fail-safe stops passed");
}
