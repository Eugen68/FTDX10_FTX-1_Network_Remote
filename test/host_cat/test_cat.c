/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Host test for main/cat.c: the CAT allow-list, the radio profiles, the setters and the answer parsers,
 * built with gcc on a PC (no board, no radio). The ESP-IDF / FreeRTOS / USB headers are replaced by the
 * minimal stubs in stub/. Run: test/host_cat/run.sh
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
char g_sent[1024];
void (*g_cdc_send_hook)(const char *, size_t);
void (*g_queue_send_hook)(const void *);
#define TEST_CAT_CAPTURE_QUEUE
#define TEST_CAT_ADVANCE_TIME
int64_t g_now_us = 1000000;
#include "cat.c"   // the real file, built against the stubs in stub/
#include "tx_swr.h"
uint8_t g_usb_addrs[10];
usb_device_desc_t g_usb_desc[128];
int g_usb_count;
esp_err_t (*g_cdc_open_hook)(const cdc_acm_host_open_config_t *, cdc_acm_dev_hdl_t *);
esp_err_t (*g_cdc_close_hook)(cdc_acm_dev_hdl_t);
esp_err_t (*g_cdc_request_hook)(cdc_acm_dev_hdl_t, uint8_t, uint16_t, uint8_t *);
uint32_t g_write_timeout_ms;
int64_t g_write_duration_us;
esp_err_t g_write_result;

static bool tx_enabled, tx_password;
static bool mock_allow_reset = true;
static unsigned mock_resets;
void cat_cdc_probe_begin(void) {}
cat_cdc_probe_t cat_cdc_probe_end(void) { return (cat_cdc_probe_t){0}; }
bool usb_audio_recover_cat(unsigned attempt)
{
    assert(attempt == atomic_load(&s_cat_recoveries) + 1);
    if (!mock_allow_reset) return false;
    ++mock_resets;
    cat_usb_reset_begin();
    g_now_us += 500000;
    cat_usb_reset_end();
    return true;
}
bool app_config_rigctl_enabled(void) { return false; }
bool app_config_yaesu_enabled(void) { return false; }
bool app_config_tx_enabled(void) { return tx_enabled && tx_password; }

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool ok(const char *c) { return cat_cmds_allowed(c, 1); }

static void use(const char *id) {
    memset(s_rig_id, 0, sizeof s_rig_id);
    if (id) { memcpy(s_rig_id, id, 4); s_rig = profile_for_id(id); } else s_rig = &s_profile_none;
}

static const char *startup_ai = "AI0;";
static bool reject_notch;
static bool fail_ai_write;
static bool reject_tx_read;

static void inject(const char *reply)
{
    rx_cb((const uint8_t *)reply, strlen(reply), (void *)(uintptr_t)atomic_load(&s_cat_session));
}

static void startup_reply(const char *cmd, size_t len)
{
    static const struct { const char *cmd, *reply; } reads[] = {
        {"FA;", "FA007130000;"}, {"FB;", "FB007100000;"},
        {"MD0;", "MD0C;"}, {"MD1;", "MD11;"},
        {"AG0;", "AG0010;"}, {"AG1;", "AG1010;"},
        {"SQ0;", "SQ0000;"}, {"SQ1;", "SQ1000;"},
        {"RI0;", "RI00000000;"}, {"TX;", "TX0;"},
        {"BY;", "BY00;"}, {"RM0;", "RM0000000;"}, {"RM1;", "RM1000000;"},
        {"BP00;", "BP00000;"}, {"BP01;", "BP01150;"},
        {"BP10;", "BP10000;"}, {"BP11;", "BP11150;"}, {"FT;", "FT0;"},
    };
    assert(memchr(cmd, ';', len) == cmd + len - 1);
    g_write_result = ESP_OK;
    if (len == 3 && memcmp(cmd, "ID;", len) == 0) {
        inject("IDABCD;ID123;FA007130000;ID0840;");
    } else if (len == 3 && memcmp(cmd, "AI;", len) == 0) {
        if (fail_ai_write) g_write_result = ESP_ERR_TIMEOUT;
        else if (startup_ai) inject(startup_ai);
    } else if (reject_tx_read && len == 3 && memcmp(cmd, "TX;", len) == 0) {
        reject_tx_read = false;
        inject("?;");
    } else if (reject_notch && len == 5 && memcmp(cmd, "BP00;", len) == 0) {
        inject("?;");
    } else {
        for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]); ++i) {
            if (strlen(reads[i].cmd) == len && memcmp(cmd, reads[i].cmd, len) == 0) {
                inject(reads[i].reply);
                return;
            }
        }
    }
}

static void usb_added(uint8_t addr)
{
    const usb_host_client_event_msg_t event = {.event = USB_HOST_CLIENT_EVENT_NEW_DEV, .new_dev.address = addr};
    cat_usb_event(&event, NULL);
}

static void usb_removed(uint8_t addr)
{
    const usb_host_client_event_msg_t event = {.event = USB_HOST_CLIENT_EVENT_DEV_REMOVED, .dev_removed.address = addr};
    cat_usb_event(&event, NULL);
}

static void test_startup(void)
{
    s_disconnected = false;
    g_write_result = ESP_OK;
    s_state.tx_cat = false;
    s_state.tx = false;
    atomic_store(&s_transition_ms, 0);
    s_unkey_requested = false;
    use(NULL);
    uint32_t rates[5];
    s_last_baud = 38400;
    const uint32_t expected[] = {38400, 115200, 19200, 9600, 4800};
    CHECK(baud_order(rates) == 5 && memcmp(rates, expected, sizeof(rates)) == 0, "cached baud first without retries");
    s_last_baud = 12345;
    CHECK(baud_order(rates) == 5 && rates[0] == 115200 && rates[4] == 4800, "ignore invalid NVS speed");
    CHECK(!matches_query("IDABCD;", 7, "ID;") && !matches_query("ID123;", 6, "ID;") &&
          matches_query("ID0840;", 7, "ID;"), "strict numeric ID");

    s_cat_addr = 2;
    usb_added(2);
    g_sent[0] = '\0';
    g_now_us += CAT_SETTLE_US - 1;
    CHECK(cat_tx((void *)1, "ID;") == ESP_ERR_INVALID_STATE && !g_sent[0], "no CAT byte before 3 seconds");
    ++g_now_us;
    g_cdc_send_hook = startup_reply;
    char id[RX_BUF_BYTES];
    CHECK(query((void *)1, "ID;", id, sizeof(id), 100, false) == QUERY_ANSWER &&
          strcmp(id, "ID0840;") == 0 && strcmp(g_sent, "ID;") == 0,
          "single probe ignores malformed ID and unrelated traffic");
    cat_usb_reset_begin();
    g_sent[0] = '\0';
    CHECK(cat_tx((void *)1, "ID;") == ESP_ERR_INVALID_STATE && !g_sent[0], "no CAT during USB reset");
    cat_usb_reset_end();
    usb_added(2);
    g_now_us += CAT_SETTLE_US - 1;
    CHECK(cat_tx((void *)1, "ID;") == ESP_ERR_INVALID_STATE && !g_sent[0], "new settling period after reset");
    ++g_now_us;

    use("0840");
    s_rejected_count = 0;
    reject_notch = true;
    const unsigned rejected = cat_rejected_commands();
    initialize_reads((void *)1, s_rig);
    CHECK(strcmp(g_sent, "FA;FB;MD0;MD1;AI;AG0;AG1;SQ0;SQ1;RI0;TX;RM0;BP00;BP01;BP10;BP11;FT;PA0;PA1;PA2;RA0;RG0;RG1;GT0;GT1;NL0;NL1;RL0;RL1;BC0;BC1;IS0;IS1;AI1;") == 0,
          "FTX-1 reads before first write");
    CHECK(cat_rejected_commands() == rejected + 1 && !poll_rejected("BP00;"),
          "one explicit rejection does not suppress a read");
    g_sent[0] = '\0';
    send_poll((void *)1, "BP00;BP01;");
    CHECK(strcmp(g_sent, "BP00;BP01;") == 0 && !poll_rejected("BP00;"), "retry after two rejections");
    send_poll((void *)1, "BP00;");
    CHECK(poll_rejected("BP00;"), "suppress optional read after three rejections");
    g_sent[0] = '\0';
    send_poll((void *)1, "BP00;BP01;");
    CHECK(strcmp(g_sent, "BP01;") == 0, "only skip the suppressed optional command");
    g_now_us += CAT_REJECT_RETRY_US - 1;
    CHECK(poll_rejected("BP00;"), "do not retry before cooldown expires");
    ++g_now_us;
    CHECK(!poll_rejected("BP00;"), "retry after 60 seconds");
    send_poll((void *)1, "BP00;");
    CHECK(poll_rejected("BP00;"), "a rejected retry starts a new cooldown");
    g_now_us += CAT_REJECT_RETRY_US;
    reject_notch = false;
    send_poll((void *)1, "BP00;");
    reject_notch = true;
    send_poll((void *)1, "BP00;");
    CHECK(!poll_rejected("BP00;"), "a successful retry clears the streak");

    use("0761");
    s_rejected_count = 0;
    reject_notch = false;
    startup_ai = "AI1;";
    g_sent[0] = '\0';
    initialize_reads((void *)1, s_rig);
    CHECK(strcmp(g_sent, "FA;FB;MD0;MD1;AI;AG0;SQ0;BY;TX;RM1;BP00;BP01;FT;PA0;RA0;RG0;GT0;NB0;NL0;NR0;RL0;BC0;IS0;") == 0, "FTDX10 leaves AI1 alone");
    startup_ai = NULL;
    g_sent[0] = '\0';
    const int64_t before = g_now_us;
    initialize_reads((void *)1, s_rig);
    CHECK(strcmp(g_sent, "FA;FB;MD0;MD1;AI;AG0;SQ0;BY;TX;RM1;BP00;BP01;FT;PA0;RA0;RG0;GT0;NB0;NL0;NR0;RL0;BC0;IS0;AI1;") == 0 &&
          g_now_us - before >= ANSWER_TIMEOUT_MS * 1000, "silent AI enables it after timeout and all reads");
    startup_ai = "AI2;";
    g_sent[0] = '\0';
    initialize_reads((void *)1, s_rig);
    CHECK(strstr(g_sent, "AI;") && !strstr(g_sent, "AI1;"), "unexpected AI reply is not silence");
    fail_ai_write = true;
    g_sent[0] = '\0';
    initialize_reads((void *)1, s_rig);
    CHECK(strstr(g_sent, "AI;") && !strstr(g_sent, "AI1;"), "USB write failure is not a radio response timeout");
    fail_ai_write = false;
    startup_ai = "?;";
    g_sent[0] = '\0';
    initialize_reads((void *)1, s_rig);
    CHECK(strstr(g_sent, "AI;") && !strstr(g_sent, "AI1;"), "rejected AI never enables it");
    g_cdc_send_hook = NULL;
}

static void test_rejected_tx_read_after_unkey(void)
{
    use("0761");
    tx_enabled = tx_password = true;
    s_ptt_lock = s_ptt_port = (void *)1;
    s_ptt_ready = true;
    atomic_store(&s_link_starting, false);
    s_disconnected = false;
    g_cdc_send_hook = startup_reply;
    s_state.tx_cat = true;
    inject("TX1;");
    g_sent[0] = '\0';
    CHECK(cat_ptt(false) == ESP_OK, "send TX0 normally");
    const int64_t unkey_at = g_now_us;
    ++g_now_us;
    const unsigned rejects = atomic_load(&s_effective_rejects);
    reject_tx_read = true;
    send_poll((void *)1, "TX;");
    CHECK(!poll_rejected("TX;") && atomic_load(&s_effective_rejects) == rejects,
          "ignore an isolated RX transition rejection");
    g_now_us += 100000;
    send_poll((void *)1, "TX;");
    CHECK(strcmp(g_sent, "TX0;TX;TX;") == 0 && cat_radio_rx_after(unkey_at),
          "next TX read confirms RX");
    CHECK(cat_ptt(true) == ESP_OK, "PTT remains available after the rejected read");
    CHECK(cat_ptt(false) == ESP_OK, "return to RX");
    tx_enabled = tx_password = false;
    g_cdc_send_hook = NULL;
    atomic_store(&s_transition_ms, 0);

    const char *essential[] = {"TX;", "RI0;", "BY;", "RM5;", "RM6;", "FT;",
                               "MD0;", "MD1;", "FA;", "FB;", "EX010113;"};
    for (size_t i = 0; i < sizeof essential / sizeof *essential; ++i) {
        for (int j = 0; j < CAT_REJECT_LIMIT; ++j) update_poll_rejections(essential[i], QUERY_REJECTED, false);
        CHECK(!poll_rejected(essential[i]), "%s remains eligible after repeated rejection", essential[i]);
    }
    s_rejected_count = 0;
    for (int i = 0; i < CAT_REJECT_LIMIT + 1; ++i) update_poll_rejections("RM4;", QUERY_REJECTED, true);
    CHECK(!poll_rejected("RM4;"), "transition rejections do not suppress optional reads");
    CHECK(ok("RM4;") && !ok("RM40;"), "ALC is read only");
    inject("RM4042000;");
    const cat_tx_meters_t meters = cat_get_tx_meters();
    CHECK(meters.alc == 42 && meters.alc_us == g_now_us, "ALC has an independent timestamp");
    inject("RM4256000;RM4042001;");
    CHECK(cat_get_tx_meters().alc == 42, "ignore invalid ALC replies");
}

static unsigned open_attempts[16], open_attempt_count, closes;
static uint8_t open_addr;
static uint32_t mock_baud;
static int64_t baud_changed_us;
static bool swap_to_ftx1, fail_first_open, suppress_id;
static unsigned cached_attempts, other_attempts, boot_probes_left;

static esp_err_t swap_open(const cdc_acm_host_open_config_t *config, cdc_acm_dev_hdl_t *out)
{
    assert(!open_addr);
    open_attempts[open_attempt_count++] = config->dev_addr;
    if (fail_first_open && config->dev_addr == 2) return ESP_ERR_NOT_FOUND;
    open_addr = config->dev_addr;
    *out = (void *)(uintptr_t)open_addr;
    return ESP_OK;
}

static esp_err_t swap_close(cdc_acm_dev_hdl_t dev)
{
    assert(open_addr == (uintptr_t)dev);
    open_addr = 0;
    ++closes;
    return ESP_OK;
}

static esp_err_t swap_request(cdc_acm_dev_hdl_t dev, uint8_t request, uint16_t length, uint8_t *data)
{
    assert(open_addr == (uintptr_t)dev);
    if (request == CP210X_SET_BAUDRATE && length == sizeof(mock_baud)) {
        memcpy(&mock_baud, data, length);
        baud_changed_us = g_now_us;
        const uint32_t before = s_rx_bytes;
        inject("\x7e\x8e");
        CHECK(s_rx_bytes == before, "early CP210x bytes are ignored after baud change");
    }
    return ESP_OK;
}

static void swap_reply(const char *cmd, size_t length)
{
    assert(cat_usb_ready());
    assert(g_now_us - baud_changed_us >= CAT_BAUD_SETTLE_MS * 1000LL);
    assert(length == 3 && memcmp(cmd, "ID;", 3) == 0);
    if (mock_baud == s_last_baud) ++cached_attempts;
    else ++other_attempts;
    if (suppress_id || boot_probes_left) {
        if (boot_probes_left) --boot_probes_left;
        inject("x9;");
        return;
    }
    const uint32_t baud = swap_to_ftx1 ? 38400 : 115200;
    if (mock_baud == baud) inject(swap_to_ftx1 ? "?;ID0840;" : "?;ID0761;");
}

static void test_hot_swap(void)
{
    s_disconnected = false;
    use(NULL);
    atomic_store(&s_link_starting, true);
    s_last_baud = 115200;
    g_cdc_open_hook = swap_open;
    g_cdc_close_hook = swap_close;
    g_cdc_request_hook = swap_request;
    g_cdc_send_hook = swap_reply;
    g_write_result = ESP_OK;
    g_write_duration_us = 0;

    g_usb_addrs[0] = 2;
    g_usb_count = 1;
    g_usb_desc[2].idVendor = SILABS_VID;
    usb_added(2);
    CHECK(!find_cat_port() && !open_attempt_count, "skip CP210x before 3 seconds without opening CDC");
    g_now_us += CAT_SETTLE_US;
    cdc_acm_dev_hdl_t dev = find_cat_port();
    CHECK(dev && s_rig->rig == CAT_RIG_FTDX10 && s_cat_addr == 2, "FTDX10 at first USB address");
    const unsigned old_session = atomic_load(&s_cat_session);
    for (int i = 0; i < CAT_REJECT_LIMIT; ++i) update_poll_rejections("BP00;", QUERY_REJECTED, false);
    CHECK(poll_rejected("BP00;"), "optional read is suppressed before hot swap");
    s_split_keyed_polls = true;

    usb_removed(2);
    CHECK(s_disconnected && !cat_usb_ready(), "hot removal invalidates the active address immediately");
    close_cat_port(&dev);
    CHECK(!dev && closes == 1 && !open_addr && !s_cat_addr && !s_cat_session, "release the old CDC handle and session");
    CHECK(!poll_rejected("BP00;") && !s_split_keyed_polls, "clear previous radio poll suppression");
    CHECK(s_last_baud == 115200, "keep remembered baud as a hint");

    swap_to_ftx1 = true;
    fail_first_open = true;
    use(NULL);
    g_usb_addrs[1] = 3; // non-Silicon Labs device should be skipped before CDC open
    g_usb_addrs[2] = 4;
    g_usb_desc[4].idVendor = SILABS_VID;
    g_usb_count = 3;
    usb_added(2);
    usb_added(4);
    g_now_us += CAT_SETTLE_US;
    g_sent[0] = '\0';
    const unsigned cached_before = cached_attempts, other_before = other_attempts;
    for (int i = 0; i < 5; ++i) {
        CHECK(!find_cat_port() && !open_addr, "keep trying cached speed before fallback");
        g_now_us += 1000000;
    }
    CHECK(cached_attempts == cached_before + 5 && other_attempts == other_before,
          "slow-start protection probes no alternate speed during the wait");
    g_now_us = s_cat_missing_since + CAT_CACHED_ONLY_US - 1;
    CHECK(cat_cached_only(g_now_us), "use cached speed for the full ten seconds");
    ++g_now_us;
    g_sent[0] = '\0';
    dev = find_cat_port();
    CHECK(dev && s_cat_addr == 4 && s_rig->rig == CAT_RIG_FTX1, "find the second FTX-1 CP210x after the first open fails");
    CHECK(s_last_baud == 38400 && strcmp(g_sent, "ID;ID;") == 0,
          "probe directly with ID at each speed, then find the new baud");
    const cdc_acm_host_dev_event_data_t delayed = {.type = CDC_ACM_HOST_DEVICE_DISCONNECTED};
    event_cb(&delayed, (void *)(uintptr_t)old_session);
    CHECK(!s_disconnected && s_cat_addr == 4 && cat_usb_ready(), "ignore a disconnect from the old CDC session");
    static const char stale_rx[] = "FA099999999;";
    rx_cb((const uint8_t *)stale_rx, sizeof(stale_rx) - 1, (void *)(uintptr_t)old_session);
    CHECK(s_state.freq_main != 99999999, "ignore data callbacks from the old CDC session");
    close_cat_port(&dev);
    CHECK(!dev && !open_addr && !s_cat_addr, "close the replacement handle");

    fail_first_open = false;
    suppress_id = true;
    use(NULL);
    open_attempt_count = 0;
    const unsigned closes_before = closes;
    CHECK(!find_cat_port() && !open_addr && closes == closes_before + 2, "release both ports after no ID");
    CHECK(open_attempt_count == 2 && open_attempts[0] == 2 && open_attempts[1] == 4,
          "first failed scan probes CP210x 2 then 4");
    CHECK(!find_cat_port() && !open_addr && closes == closes_before + 4, "release both ports in rotated scan");
    CHECK(open_attempt_count == 4 && open_attempts[2] == 4 && open_attempts[3] == 2,
          "second scan starts with the other CP210x");
    CHECK(s_last_baud == 38400, "failed scans do not overwrite the cached baud");
    static const int64_t expected_delays[] = {15000000, 30000000, 60000000, 120000000, 300000000, 300000000};
    for (unsigned i = 0; i < sizeof(expected_delays) / sizeof(*expected_delays); ++i) {
        s_cat_missing_since = g_now_us;
        g_now_us += expected_delays[i] - 1;
        recover_missing_cat(g_now_us);
        CHECK(mock_resets == i, "wait for the scheduled recovery interval");
        ++g_now_us;
        mock_allow_reset = false;
        recover_missing_cat(g_now_us);
        CHECK(mock_resets == i && s_cat_recoveries == i, "TX guard defers recovery without using an attempt");
        mock_allow_reset = true;
        recover_missing_cat(g_now_us);
        CHECK(mock_resets == i + 1 && !cat_usb_ready(), "missing ID triggers power cycle and a new settling period");
        CHECK(!cat_usb_address_ready(2) && !cat_usb_address_ready(4), "forget prior USB enumeration");
        cat_state_t state;
        cat_get_state(&state);
        CHECK(state.waiting_for_cat && state.cat_usb_resets == i + 1, "expose the recovery count while CAT is missing");
    }
    usb_added(2);
    usb_added(4);
    g_now_us += CAT_SETTLE_US;
    suppress_id = false;
    dev = find_cat_port();
    CHECK(dev && s_rig->rig == CAT_RIG_FTX1 && !s_cat_recoveries && !s_cat_waiting,
          "valid ID clears the recovery counter and waiting marker");
    close_cat_port(&dev);
    CHECK(!ok(";") && !ok(";ID;"), "parser delimiter is not in the normal CAT allow-list");
    s_scan_after_addr = 0;
    g_usb_count = 0;
    g_cdc_open_hook = NULL;
    g_cdc_close_hook = NULL;
    g_cdc_request_hook = NULL;
    g_cdc_send_hook = NULL;
    atomic_store(&s_link_starting, false);
}

static void test_slow_radio_power_on(void)
{
    atomic_store(&s_link_starting, true);
    g_cdc_open_hook = swap_open;
    g_cdc_close_hook = swap_close;
    g_cdc_request_hook = swap_request;
    g_cdc_send_hook = swap_reply;
    s_scan_after_addr = 0;
    g_usb_addrs[0] = 2;
    g_usb_count = 1;

    for (unsigned rig = 0; rig < 2; ++rig) {
        swap_to_ftx1 = rig == 1;
        s_last_baud = swap_to_ftx1 ? 38400 : 115200;
        for (unsigned cycle = 0; cycle < 5; ++cycle) {
            use(NULL);
            open_attempt_count = 0;
            s_cat_missing_since = 0;
            boot_probes_left = 4;
            const unsigned alternate_before = other_attempts;
            const unsigned recovery_before = mock_resets;
            usb_added(2);
            CHECK(!find_cat_port() && open_attempt_count == 0, "each hot start observes the 3 s guard");
            g_now_us += CAT_SETTLE_US;
            for (unsigned retry = 0; retry < 4; ++retry) {
                CHECK(!find_cat_port(), "cached probe while radio is booting");
                g_now_us += CAT_CACHED_SCAN_MS * 1000LL;
            }
            cdc_acm_dev_hdl_t dev = find_cat_port();
            CHECK(dev && s_cat_addr == 2 && open_attempt_count == 5, "CAT connects on the fifth cached probe");
            CHECK(s_rig->rig == (swap_to_ftx1 ? CAT_RIG_FTX1 : CAT_RIG_FTDX10),
                  "identify the correct radio after slow boot");
            CHECK(other_attempts == alternate_before && mock_resets == recovery_before,
                  "delayed CAT needs neither alternate baud nor a power cycle");
            close_cat_port(&dev);
            usb_removed(2);
        }
    }
    g_usb_count = 0;
    g_cdc_open_hook = NULL;
    g_cdc_close_hook = NULL;
    g_cdc_request_hook = NULL;
    g_cdc_send_hook = NULL;
    atomic_store(&s_link_starting, false);
}

static cat_cmd_item_t queued_rigctl;

static void capture_cat_item(const void *item)
{
    queued_rigctl = *(const cat_cmd_item_t *)item;
}

static void rigctl_readback(const char *cmd, size_t len)
{
    const char *answer = NULL;
    if (len == 3 && memcmp(cmd, "VS;", len) == 0) answer = "VS1;";
    if (len == 3 && memcmp(cmd, "FT;", len) == 0) answer = "FT0;";
    if (len == 3 && memcmp(cmd, "FA;", len) == 0) answer = "FA007025000;";
    if (len == 3 && memcmp(cmd, "FB;", len) == 0) answer = "FB007030000;";
    if (len == 4 && memcmp(cmd, "MD0;", len) == 0) answer = "MD03;";
    if (len == 4 && memcmp(cmd, "MD1;", len) == 0) answer = "MD13;";
    if (len == 4 && memcmp(cmd, "SH1;", len) == 0) answer = "SH1010;";
    if (answer) inject(answer);
}

static void check_rigctl_wire(const char *expected)
{
    g_sent[0] = '\0';
    CHECK(run_rigctl_write((void *)1, queued_rigctl), "radio must confirm %s", expected);
    CHECK(strcmp(g_sent, expected) == 0, "expected %s, sent %s", expected, g_sent);
}

static void test_rigctl_cat_writes(void)
{
    use("0761");
    s_disconnected = false;
    atomic_store(&s_link_starting, false);
    publish_profile();
    inject("VS0;FT1;SH0013;TX0;");
    cat_state_t state;
    cat_get_state(&state);
    CHECK(state.rx_vfo_known && !state.rx_vfo_sub && state.tx_vfo == CAT_TX_VFO_SUB &&
          state.width_code[0] == 13, "NET getters use cached VS/FT/SH");
    CHECK(cat_rigctl_set_freq(false, 14074000, true, 2) == ESP_OK, "fresh RX permits tuning");
    inject("TX1;");
    CHECK(cat_rigctl_set_freq(false, 14074000, true, 2) == ESP_ERR_INVALID_STATE, "reject during external TX");
    const cat_cmd_item_t item = {
        .cmd = "FA014074000;", .rig = CAT_RIG_FTDX10, .ticket = 2, .require_active = true,
        .expires_us = g_now_us + 1000000
    };
    g_sent[0] = '\0';
    CHECK(!run_rigctl_write((void *)1, item) && !g_sent[0], "queued SET is rejected when TX starts");
    inject("TX0;VS1;SH0017;");
    CHECK(s_state.width_code[1] == 17, "associate SH0 with selected FTDX10 VFO");
    CHECK(!run_rigctl_write((void *)1, item) && !g_sent[0], "queued SET cannot target previous VFO");
    g_write_result = ESP_OK;
    g_write_duration_us = 0;
    g_queue_send_hook = capture_cat_item;
    g_cdc_send_hook = rigctl_readback;
    const char *radios[] = {"0761", "0840"};
    for (unsigned i = 0; i < 2; ++i) {
        use(radios[i]);
        publish_profile();
        // Roll from 21 MHz to 7 MHz on A while B stays active.
        inject("VS1;FA021025000;TX0;");
        CHECK(cat_rigctl_set_freq(false, 7025000, false, 2) == ESP_OK, "explicit A can be inactive");
        check_rigctl_wire("FA007025000;FA;");
        CHECK(s_state.rx_vfo_sub && s_state.freq_main == 7025000, "A updates without changing active B");
        CHECK(cat_rigctl_set_mode(false, '3', -1, false, 2) == ESP_OK, "explicit A mode");
        check_rigctl_wire("MD03;MD0;");
        inject("VS0;TX0;");
        CHECK(cat_rigctl_set_freq(true, 7030000, false, 2) == ESP_OK, "explicit B can be inactive");
        check_rigctl_wire("FB007030000;FB;");
        CHECK(cat_rigctl_set_mode(true, '3', 0, false, 2) == ESP_OK, "explicit B normal filter");
        check_rigctl_wire("MD13;MD1;");
        CHECK(!s_state.rx_vfo_sub && s_state.mode_sub_raw == '3', "B updates without changing active A");

        CHECK(cat_rigctl_set_mode(true, '3', 500, false, 2) == ESP_OK, "queue explicit filter width");
        if (s_rig->dual_rx) {
            check_rigctl_wire("MD13;MD1;SH1010;SH1;");
        } else {
            g_sent[0] = '\0';
            CHECK(!run_rigctl_write((void *)1, queued_rigctl) && !g_sent[0],
                  "FTDX10 cannot set SH0 on inactive B");
        }
        CHECK(cat_rigctl_set_split(true, false, false, 2) == ESP_OK, "queue split with named inactive RX");
        check_rigctl_wire(s_rig->dual_rx ? "VS1;VS;FT0;FT;" : "VS1;VS;FT2;FT;");
        CHECK(s_state.rx_vfo_sub && s_state.tx_vfo == CAT_TX_VFO_MAIN, "split receives on B and transmits on A");
    }
    g_cdc_send_hook = NULL;
    g_queue_send_hook = NULL;
    use("0840");
    inject("VS1;SH1009;SH0013;");
    CHECK(s_state.width_code[0] == 13 && s_state.width_code[1] == 9, "FTX-1 has independent SH codes");
}

int main(void) {
    s_lock = (void*)1; s_cmd_queue = (void*)1;
    // profile tables: every polled string must pass its own allow-list
    for (size_t i = 0; i < sizeof s_profiles / sizeof s_profiles[0]; i++) {
        s_rig = &s_profiles[i];
        const char *polls[] = {s_rig->sync_all, s_rig->poll_fast, s_rig->poll_status, s_rig->resync_vfo};
        for (int k = 0; k < 4; k++) if (polls[k]) CHECK(cat_cmds_allowed(polls[k], CAT_TX_MAX_CMDS), "%s poll %s", s_rig->name, polls[k]);
        char ex[16]; snprintf(ex, sizeof ex, "EX%s100;", s_rig->ex_data_out); CHECK(ok(ex), "%s %s", s_rig->name, ex);
    }
    s_rig = &s_profile_unknown;
    CHECK(cat_cmds_allowed(s_rig->sync_all, 16) && cat_cmds_allowed(s_rig->poll_status, 16), "unknown polls");

    // nothing before identification except ID;
    use(NULL);
    CHECK(ok("ID;"), "ID before ident");
    CHECK(!ok("FA;") && !ok("AI1;") && !ok("FA014074000;"), "none profile blocks");

    CHECK(ok("TX0;") && !ok("TX1;") && !ok("TX;"), "unkey allowed even before identification");
    CHECK(!ok("TX00;") && !ok("TX01;"), "only the exact TX0 command is unconditional");

    // never: transmit commands on any profile
    const char *never[] = {"TX1;", "TX2;", "MX1;", "KY0;", "VX1;", "PC100;", "EX010411100;x", "AI0;", NULL};
    const char *ids[] = {"0840", "0761", "9999", NULL};
    for (int r = 0; ids[r]; r++) { use(ids[r]); for (int k = 0; never[k]; k++) CHECK(!ok(never[k]), "%s allowed %s", ids[r], never[k]); }


    use("0840");
    CHECK(ok("FT0;") && ok("FT1;") && !ok("FT2;") && !ok("FT3;"), "FTX-1 TX VFO selection codes");
    CHECK(ok("VS;") && ok("VS1;") && !ok("VS2;"), "VFO selection restricted to 0/1");
    CHECK(ok("SH1;") && ok("SH1023;") && !ok("SH1024;"), "FTX-1 SUB permits bounded SH");
    use("0761");
    CHECK(!ok("FT0;") && !ok("FT1;") && ok("FT2;") && ok("FT3;"), "FTDX10 TX VFO selection codes");
    CHECK(ok("SH0013;") && !ok("SH1;") && !ok("SH1013;"), "FTDX10 SH fixed receiver field");
    use("9999");
    CHECK(!ok("FT0;") && !ok("FT2;") && !ok("VS1;") && !ok("SH0013;"), "unknown rig cannot set VFO/filter");

    // front-end: pre-amp, attenuator, RF gain, AGC, each with the radio's own parameter ranges
    use("0840");
    CHECK(ok("PA0;") && ok("PA1;") && ok("PA2;") && !ok("PA3;") && !ok("PA;"), "FTX-1 PA bands");
    CHECK(ok("PA02;") && !ok("PA03;") && ok("PA11;") && !ok("PA12;") && ok("PA21;") && !ok("PA22;"), "FTX-1 PA values");
    CHECK(ok("RA0;") && ok("RA01;") && !ok("RA02;") && !ok("RA1;") && !ok("RA11;"), "FTX-1 ATT on/off");
    CHECK(ok("RG1;") && ok("RG1255;") && !ok("RG1256;") && !ok("RG2100;") && !ok("RG112;"), "FTX-1 RF gain");
    CHECK(ok("GT1;") && ok("GT14;") && !ok("GT15;") && !ok("GT2;") && !ok("GT1A;"), "FTX-1 AGC");
    use("0761");
    CHECK(ok("PA0;") && ok("PA02;") && !ok("PA1;") && !ok("PA11;") && !ok("PA03;"), "FTDX10 PA");
    CHECK(ok("RA03;") && !ok("RA04;"), "FTDX10 ATT 6/12/18 dB");
    CHECK(ok("RG0128;") && !ok("RG1;") && !ok("RG1128;"), "FTDX10 RF gain, one receiver");
    CHECK(ok("GT04;") && !ok("GT1;") && !ok("GT10;"), "FTDX10 AGC, one receiver");
    use("9999");
    CHECK(!ok("PA0;") && !ok("RA0;") && !ok("RG0;") && !ok("GT0;") && !ok("PA00;"), "unknown rig: no front-end");

    // noise: NB / DNR / DNF / IF shift
    use("0840");
    CHECK(!ok("NB0;") && !ok("NB01;") && !ok("NR01;"), "FTX-1 has no NB / NR switch");
    CHECK(ok("NL1;") && ok("NL1010;") && ok("NL0000;") && !ok("NL1011;") && !ok("NL2005;"), "FTX-1 NB level");
    CHECK(ok("RL1;") && ok("RL100;") && ok("RL010;") && !ok("RL011;") && !ok("RL01;"), "FTX-1 DNR level 00-10");
    CHECK(ok("BC1;") && ok("BC11;") && !ok("BC12;") && !ok("BC2;"), "FTX-1 DNF");
    CHECK(ok("IS1;") && ok("IS10+1200;") && ok("IS00-0020;") && !ok("IS00+1220;") && !ok("IS00+0010;") &&
          !ok("IS01+0100;") && !ok("IS00 0100;") && !ok("IS00+100;"), "FTX-1 IF shift");
    use("0761");
    CHECK(ok("NB0;") && ok("NB01;") && !ok("NB02;") && !ok("NB1;") && ok("NR00;"), "FTDX10 NB / NR switch");
    CHECK(ok("NL0010;") && !ok("NL1005;") && ok("RL015;") && !ok("RL016;") && !ok("RL000;"), "FTDX10 levels");
    CHECK(ok("BC01;") && !ok("BC11;") && ok("IS00-1200;") && !ok("IS10+0000;"), "FTDX10 DNF / IF shift, one receiver");
    use("9999");
    CHECK(!ok("NL0;") && !ok("RL0;") && !ok("BC0;") && !ok("IS0;") && !ok("NB0;"), "unknown rig: no noise controls");
    // scope span: FTDX10 only, and only the SPAN item of SS
    use("0761");
    CHECK(ok("SS0500000;") && ok("SS0590000;") && !ok("SS05A0000;") && !ok("SS0560001;"), "FTDX10 scope span");
    CHECK(!ok("SS05;") && !ok("SS0000000;") && !ok("SS0630000;") && !ok("SS1560000;"), "FTDX10 other SS items refused");
    use("0840");
    CHECK(!ok("SS0560000;"), "FTX-1: no scope span command here");

    tx_enabled = true;
    use("0840");
    CHECK(!ok("TX1;") && ok("TX0;"), "unkey allowed without password");
    tx_password = true;
    CHECK(ok("TX1;") && ok("TX0;") && !ok("TX2;"), "FTX-1 authorized TX");
    use("0761");
    CHECK(ok("TX1;") && ok("TX0;") && !ok("TX2;") && !ok("MX1;") && !ok("KY0;") && !ok("VX1;") && !ok("PC100;"),
          "FTDX10 only allows the intended TX commands");
    CHECK(ok("EX010415;") && !ok("EX010415001;"), "TX menu read only");
    use("9999");
    CHECK(!ok("TX1;") && ok("TX0;") && !ok("EX010415;"), "unknown radio allows only the emergency PTT write");
    tx_enabled = tx_password = false;
    // FTX-1
    use("0840");
    CHECK(s_rig->rig == CAT_RIG_FTX1, "ftx1 detect");
    CHECK(ok("FA470000000;") && !ok("FA470000001;") && !ok("FA000029999;"), "ftx1 freq");
    CHECK(ok("AG1255;") && ok("SQ1255;") && ok("BP11320;") && ok("MD1H;"), "ftx1 sub + c4fm");
    CHECK(ok("EX010411100;") && !ok("EX010413100;") && !ok("EX010411101;") && !ok("EX010411;"), "ftx1 EX");
    CHECK(ok("RI0;") && ok("RM0;") && !ok("RM1;") && !ok("BY;") && ok("TX;"), "ftx1 status");

    // FTDX10
    use("0761");
    CHECK(s_rig->rig == CAT_RIG_FTDX10, "ftdx10 detect");
    CHECK(ok("FA075000000;") && !ok("FA075000001;") && !ok("FB145000000;"), "ftdx10 freq");
    CHECK(ok("AG0255;") && !ok("AG1100;") && !ok("AG1;") && !ok("SQ1;") && !ok("BP10;") && !ok("BP11100;"), "ftdx10 no sub rx");
    CHECK(ok("SQ0100;") && !ok("SQ0101;"), "ftdx10 sq max");
    CHECK(ok("MD1C;") && ok("MD0C;") && !ok("MD0H;") && !ok("MD0I;"), "ftdx10 modes");
    CHECK(ok("EX010413100;") && !ok("EX010411100;"), "ftdx10 EX: 010411 is HCUT FREQ there");
    CHECK(ok("BY;") && ok("TX;") && ok("RM1;") && !ok("RM0;") && ok("RI0;"), "ftdx10 status");

    // unknown radio: read only
    use("9999");
    CHECK(s_rig->rig == CAT_RIG_UNKNOWN, "unknown");
    CHECK(ok("FA;") && ok("MD0;") && !ok("FA014074000;") && !ok("MD02;") && !ok("AG0;") && !ok("AI1;") && !ok("PS0;") && !ok("SV;") && !ok("EX010411100;"), "unknown read-only");

    // setters
    use("0761");
    CHECK(cat_set_squelch(true, 10) == ESP_ERR_NOT_SUPPORTED, "sq sub ftdx10");
    CHECK(cat_set_squelch(false, 101) == ESP_ERR_INVALID_ARG, "sq 101 ftdx10");
    CHECK(cat_set_squelch(false, 100) == ESP_OK, "sq 100 ftdx10");
    CHECK(cat_set_volume(true, 10) == ESP_ERR_NOT_SUPPORTED, "vol sub ftdx10");
    CHECK(cat_set_freq(false, 144000000) == ESP_ERR_INVALID_ARG && cat_set_freq(true, 50313000) == ESP_OK, "freq ftdx10");
    CHECK(cat_set_mode(false, "C4FM-DN") != ESP_OK && cat_set_mode(true, "DATA-USB") == ESP_OK, "mode ftdx10");
    use("9999");
    CHECK(cat_set_freq(false, 14074000) == ESP_ERR_INVALID_STATE, "unknown set");
    use("0840");
    CHECK(cat_set_squelch(true, 255) == ESP_OK && cat_set_freq(true, 433000000) == ESP_OK, "ftx1 setters");
    CHECK(cat_set_scope_span(6) == ESP_ERR_NOT_SUPPORTED, "ftx1 scope span");
    use("0761");
    g_queue_send_hook = capture_cat_item;
    CHECK(cat_set_scope_span(6) == ESP_OK && strcmp(queued_rigctl.cmd, "SS0560000;") == 0, "ftdx10 100 kHz span");
    CHECK(cat_set_scope_span(10) == ESP_ERR_INVALID_ARG, "span code range");
    g_queue_send_hook = NULL;
    use("0840");

    // split from the page: transmitter selection only, with each radio's own FT codes
    g_queue_send_hook = capture_cat_item;
    CHECK(cat_set_split(true) == ESP_OK && strcmp(queued_rigctl.cmd, "FT1;") == 0, "ftx1 split on");
    CHECK(cat_set_split(false) == ESP_OK && strcmp(queued_rigctl.cmd, "FT0;") == 0, "ftx1 split off");
    use("0761");
    CHECK(cat_set_split(true) == ESP_OK && strcmp(queued_rigctl.cmd, "FT3;") == 0, "ftdx10 split on");
    CHECK(cat_set_split(false) == ESP_OK && strcmp(queued_rigctl.cmd, "FT2;") == 0, "ftdx10 split off");
    use("9999");
    CHECK(cat_set_split(true) == ESP_ERR_INVALID_STATE, "unknown radio: no split");
    g_queue_send_hook = NULL;
    use("0840");
    // front-end setters and answers
    g_queue_send_hook = capture_cat_item;
    CHECK(cat_set_preamp(1, 1) == ESP_OK && strcmp(queued_rigctl.cmd, "PA11;") == 0, "ftx1 VHF preamp");
    CHECK(cat_set_preamp(1, 2) == ESP_ERR_INVALID_ARG && cat_set_att(2) == ESP_ERR_INVALID_ARG, "ftx1 ranges");
    CHECK(cat_set_rf_gain(true, 7) == ESP_OK && strcmp(queued_rigctl.cmd, "RG1007;") == 0, "ftx1 sub RF gain");
    CHECK(cat_set_agc(false, 5) == ESP_ERR_INVALID_ARG, "AGC set range");
    use("0761");
    CHECK(cat_set_att(3) == ESP_OK && strcmp(queued_rigctl.cmd, "RA03;") == 0, "ftdx10 18 dB");
    CHECK(cat_set_agc(false, 4) == ESP_OK && strcmp(queued_rigctl.cmd, "GT04;") == 0, "ftdx10 AGC auto");
    CHECK(cat_set_preamp(1, 0) == ESP_ERR_NOT_SUPPORTED && cat_set_rf_gain(true, 1) == ESP_ERR_NOT_SUPPORTED,
          "ftdx10 has one receiver and no VHF band");
    use("9999");
    CHECK(cat_set_preamp(0, 0) == ESP_ERR_NOT_SUPPORTED && cat_set_agc(false, 1) == ESP_ERR_NOT_SUPPORTED, "unknown");
    use("0840");
    CHECK(cat_set_nb(true, true, 7) == ESP_OK && strcmp(queued_rigctl.cmd, "NL1007;") == 0, "ftx1 NB level");
    CHECK(cat_set_nb(true, false, CAT_LEVEL_KEEP) == ESP_OK && strcmp(queued_rigctl.cmd, "NL1000;") == 0, "ftx1 NB off");
    CHECK(cat_set_dnr(false, true, 10) == ESP_OK && strcmp(queued_rigctl.cmd, "RL010;") == 0, "ftx1 DNR");
    CHECK(cat_set_dnr(false, true, 11) == ESP_ERR_INVALID_ARG, "ftx1 DNR max 10");
    CHECK(cat_set_dnf(true, true) == ESP_OK && strcmp(queued_rigctl.cmd, "BC11;") == 0, "ftx1 DNF");
    CHECK(cat_set_if_shift(true, -340) == ESP_OK && strcmp(queued_rigctl.cmd, "IS10-0340;") == 0, "ftx1 IF shift");
    CHECK(cat_set_if_shift(false, 1210) == ESP_ERR_INVALID_ARG && cat_set_if_shift(false, 30) == ESP_ERR_INVALID_ARG,
          "IF shift range and step");
    use("0761");
    CHECK(cat_set_nb(false, true, 4) == ESP_OK && strcmp(queued_rigctl.cmd, "NB01;") == 0, "ftdx10 NB on after level");
    CHECK(cat_set_dnr(false, false, CAT_LEVEL_KEEP) == ESP_OK && strcmp(queued_rigctl.cmd, "NR00;") == 0, "ftdx10 DNR off");
    CHECK(cat_set_dnr(false, true, 15) == ESP_OK && cat_set_dnf(true, true) == ESP_ERR_NOT_SUPPORTED, "ftdx10 DNR / no SUB");
    use("9999");
    CHECK(cat_set_if_shift(false, 0) == ESP_ERR_NOT_SUPPORTED, "unknown radio: no IF shift");
    g_queue_send_hook = NULL;
    use("0840");
    {
        cat_state_t fe;
        handle_cat_packet("PA02;");
        handle_cat_packet("PA21;");
        handle_cat_packet("RA01;");
        handle_cat_packet("RG1100;");
        handle_cat_packet("GT06;");
        handle_cat_packet("RG0999;");   // out of range: ignored
        handle_cat_packet("NL1003;");
        handle_cat_packet("RL000;");
        handle_cat_packet("BC01;");
        handle_cat_packet("IS10-0340;");
        cat_get_state(&fe);
        CHECK(fe.preamp[0] == 2 && fe.preamp[2] == 1 && fe.att == 1 && fe.rf_gain[1] == 100 && fe.agc[0] == 6 &&
              fe.rf_gain[0] != 999, "front-end answers");
        CHECK(fe.nb_on[1] == 1 && fe.nb_level[1] == 3 && fe.dnr_on[0] == 0 && fe.dnf[0] == 1 &&
              fe.if_shift_known[1] && fe.if_shift[1] == -340 && !fe.if_shift_known[0], "noise answers");
        use("0761");
        handle_cat_packet("NB01;");
        handle_cat_packet("NR01;");
        handle_cat_packet("RL012;");
        handle_cat_packet("PA01;");
        cat_get_state(&fe);
        CHECK(fe.nb_on[0] == 1 && fe.dnr_on[0] == 1 && fe.dnr_level[0] == 12 && fe.preamp[0] == 1,
              "FTDX10 NB / NR switches and levels");
        use("0840");
    }

    // parsers
    int m, sb; bool b;
    CHECK(parse_rm("RM0120060;", &m, &sb) && m == 120 && sb == 60, "rm0");
    CHECK(parse_rm("RM1123000;", &m, &sb) && m == 123 && sb == -1, "rm1");
    CHECK(!parse_rm("RM2123000;", &m, &sb) && !parse_rm("RM0256000;", &m, &sb), "rm bad");
    CHECK(parse_by("BY10;", &b) && b && parse_by("BY00;", &b) && !b && !parse_by("BY1;", &b), "by");
    CHECK(parse_tx("TX2;", &b) && b && parse_tx("TX0;", &b) && !b && !parse_tx("TX3;", &b), "tx");
    // packet handling: FTDX10 meter/busy/tx land in the state
    use("0761");
    CHECK(!cat_radio_rx_confirmed(), "recovery requires a TX status");
    handle_cat_packet("RM1200000;"); handle_cat_packet("BY10;"); handle_cat_packet("TX2;");
    CHECK(s_state.s_main == 200 && s_state.s_sub == 0 && s_state.busy && s_state.tx, "ftdx10 state");
    CHECK(!cat_radio_rx_confirmed(), "manual TX defers recovery");
    handle_cat_packet("TX1;");
    CHECK(!cat_radio_rx_confirmed(), "CAT TX defers recovery");
    handle_cat_packet("TX0;"); handle_cat_packet("BY00;");
    CHECK(!s_state.busy && !s_state.tx, "ftdx10 state clear");
    CHECK(cat_radio_rx_confirmed(), "recovery resumes with RX confirmed");
    g_now_us += RADIO_STATUS_MAX_AGE_US + 1;
    handle_cat_packet("RM1200000;");
    handle_cat_packet("RI00000000;");
    CHECK(!cat_radio_rx_confirmed(), "other replies do not refresh stale TX state");
    handle_cat_packet("TX0;");
    s_disconnected = true;
    CHECK(!cat_radio_rx_confirmed(), "CAT loss defers recovery");
    publish(false, 0, 0, 0, 0, 0, 0, NULL, 0, NULL);
    s_disconnected = false;
    use("0840");
    handle_cat_packet("RI00010000;");
    CHECK(s_state.tx && !cat_radio_rx_confirmed(), "FTX-1 TX defers recovery");
    handle_cat_packet("RI000X0000;");
    CHECK(s_state.tx && !cat_radio_rx_confirmed(), "invalid FTX-1 status does not clear TX");
    handle_cat_packet("RI00000000;");
    CHECK(cat_radio_rx_confirmed(), "FTX-1 confirmed RX permits recovery");
    CHECK(!cat_radio_rx_after(0), "FTX-1 RI alone cannot confirm CAT PTT is off");
    handle_cat_packet("TX0;");
    CHECK(cat_radio_rx_after(0) && !cat_radio_rx_after(g_now_us), "fresh TX0 status confirms RX");
    handle_cat_packet("RI01010000;");
    CHECK(strcmp(cat_ptt_fault(g_now_us, 1500000), "High SWR") == 0, "FTX-1 RI reports high SWR");
    use("0761");
    ++g_now_us;
    handle_cat_packet("RI00010000;");
    CHECK(s_swr_status_us < g_now_us, "FTDX10 does not use extended RI replies");
    ++g_now_us;
    handle_cat_packet("RI02;");
    CHECK(s_swr_status_us < g_now_us, "invalid FTDX10 SWR flag does not refresh RI status");
    handle_cat_packet("RI01;");
    CHECK(s_swr_status_us == g_now_us, "FTDX10 short RI reply refreshes SWR status");
    const int64_t spike_start = g_now_us - 1000;
    CHECK(cat_ptt_fault(spike_start, 1500000) == NULL, "FTDX10 rechecks early Hi-SWR");
    g_now_us = spike_start + FTDX10_HI_SWR_STARTUP_US - 1;
    CHECK(cat_ptt_fault(spike_start, 1500000) == NULL, "FTDX10 gives the startup flag time to clear");
    ++g_now_us;
    CHECK(strcmp(cat_ptt_fault(spike_start, 1500000), "High SWR") == 0,
          "FTDX10 stops when startup Hi-SWR has no timely normal reply");
    handle_cat_packet("RI00;");
    CHECK(strcmp(cat_ptt_fault(spike_start, 1500000), "High SWR") == 0,
          "a normal reply at the deadline is too late to clear the startup fault");

    const int64_t next_start = ++g_now_us;
    g_now_us += 100000;
    handle_cat_packet("RI01;");
    g_now_us += 100000;
    handle_cat_packet("RI00;");
    g_now_us = next_start + FTDX10_HI_SWR_STARTUP_US;
    CHECK(cat_ptt_fault(next_start, 1500000) == NULL, "fresh RI00 suppresses only the startup spike");
    handle_cat_packet("RI01;");
    ++g_now_us;
    handle_cat_packet("RI00;");
    CHECK(strcmp(cat_ptt_fault(next_start, 1500000), "High SWR") == 0,
          "post-startup Hi-SWR latches even if followed by RI00");
    use("0840");
    handle_cat_packet("RI00000000;");

    handle_cat_packet("RI00000000 1;"); // malformed: ignored
    // Meter ballistics: rise at once, fall a quarter of the gap per reading, settle after a few readings
    handle_cat_packet("RM0010020;");
    CHECK(s_state.s_main == 153 && s_state.s_sub == 20, "meter after one reading: %u %u", s_state.s_main, s_state.s_sub);
    for (int i = 0; i < 30; i++) handle_cat_packet("RM0010020;");
    CHECK(s_state.s_main == 10 && s_state.s_sub == 20, "meter settled: %u %u", s_state.s_main, s_state.s_sub);
    handle_cat_packet("RM0200020;");
    CHECK(s_state.s_main == 200, "meter rises at once: %u", s_state.s_main);

    cat_state_t reported;
    CHECK(ok("FT;"), "FTX-1 TX VFO query");
    handle_cat_packet("FT0;");
    cat_get_state(&reported);
    CHECK(reported.tx_vfo == CAT_TX_VFO_MAIN, "FTX-1 reports MAIN transmitter");
    use("0761");
    CHECK(ok("FT;"), "FTDX10 TX VFO query");
    handle_cat_packet("FT1;");
    handle_cat_packet("FT3;");
    cat_get_state(&reported);
    CHECK(reported.tx_vfo == CAT_TX_VFO_SUB, "FTDX10 reports SUB transmitter");
    g_now_us += RADIO_STATUS_MAX_AGE_US + 1;
    cat_get_state(&reported);
    CHECK(reported.tx_vfo == CAT_TX_VFO_UNKNOWN, "stale selection cannot color a VFO");
    use("9999");
    CHECK(!ok("FT;"), "unknown radio cannot query TX VFO");

    use("0840");
    handle_cat_packet("FT0;");
    const char permitted[] = "128C";
    const char refused[] = "345679ABDEFHI";
    char mode_packet[] = "MD00;";
    for (size_t i = 0; i < sizeof(permitted) - 1; i++) {
        mode_packet[3] = permitted[i];
        handle_cat_packet(mode_packet);
        CHECK(cat_ptt_mode_error(0) == NULL, "MAIN mode %c permits PTT", permitted[i]);
    }
    for (size_t i = 0; i < sizeof(refused) - 1; i++) {
        mode_packet[3] = refused[i];
        handle_cat_packet(mode_packet);
        CHECK(cat_ptt_mode_error(0) != NULL, "MAIN mode %c must block PTT", refused[i]);
    }
    handle_cat_packet("MD02;");
    handle_cat_packet("FT1;");
    handle_cat_packet("MD14;");
    CHECK(cat_ptt_mode_error(0) != NULL, "SUB FM must block PTT when SUB was selected for TX");
    handle_cat_packet("MD18;");
    CHECK(cat_ptt_mode_error(0) == NULL, "SUB DATA-LSB is permitted");
    g_now_us += RADIO_STATUS_MAX_AGE_US + 1;
    CHECK(cat_ptt_mode_error(0) != NULL, "stale mode must block PTT");
    handle_cat_packet("MD02;");
    CHECK(cat_ptt_mode_error(0) != NULL, "unconfirmed TX VFO must block PTT");
    handle_cat_packet("FT0;");
    CHECK(cat_set_mode_char(false, '5') == ESP_OK, "queue an AM mode change");
    CHECK(cat_ptt_mode_error(0) != NULL, "optimistic mode change cannot authorize PTT");

    use("0761");
    CHECK(ok("RM5;") && ok("RM6;") && !ok("RM60;") && !ok("RM7;"), "PO/SWR are read-only");
    handle_cat_packet("RM5022000;");
    handle_cat_packet("RM6126000;");
    handle_cat_packet("RM6256000;");
    handle_cat_packet("RM6111111;");
    cat_tx_meters_t meters = cat_get_tx_meters();
    CHECK(meters.rig == CAT_RIG_FTDX10 && meters.po == 22 && meters.swr == 126, "FTDX10 meters");
    use("0840");
    CHECK(ok("RM5;") && ok("RM6;"), "FTX-1 PO/SWR");
    ++g_now_us;
    handle_cat_packet("RM5010000;");
    handle_cat_packet("RM6089000;");
    meters = cat_get_tx_meters();
    CHECK(meters.rig == CAT_RIG_FTX1 && meters.swr == 89, "FTX-1 SWR");
    use("9999");
    CHECK(!ok("RM5;") && !ok("RM6;"), "unknown radio blocks transmit meters");

    use("0761");
    handle_cat_packet("TX1;");
    cat_get_state(&reported);
    CHECK(reported.tx_radio_known && reported.tx_radio, "fresh CAT TX report");
    g_now_us += RADIO_STATUS_MAX_AGE_US + 1;
    handle_cat_packet("FA014074000;");
    cat_get_state(&reported);
    CHECK(!reported.tx_radio_known && !reported.tx_radio, "other CAT replies do not refresh TX state");

    const int64_t last_pair = meters.swr_us;
    ++g_now_us;
    handle_cat_packet("RM6100000;");
    CHECK(cat_get_tx_meters().swr_us == last_pair, "RM6 alone cannot refresh a metering pair");

    cat_write_timing_t timing = {0};
    capture_write_timing(&timing, 200000, ESP_ERR_TIMEOUT, TX_TIMEOUT_MS);
    capture_write_timing(&timing, 201000, ESP_OK, TX_TIMEOUT_MS);
    CHECK(timing.calls == 2 && timing.timeouts == 1 && timing.over_budget == 1 && timing.max_us == 201000,
          "measure timeout and duration independently");
    report_queued_write_failure("MD02;", ESP_ERR_TIMEOUT);
    cat_get_state(&reported);
    CHECK(reported.write_error_seq == 1 && strstr(reported.write_error, "MD02;"),
          "failed command is visible in the web status");

    // CAT log: blocked commands are logged as BLOCKED and never reach the port; reads come back oldest first
    use("0840");
    g_sent[0] = '\0';
    cat_log_entry_t le[200];
    const size_t before = cat_log_read(0, le, 200);
    const uint32_t mark = before ? le[before - 1].seq : 0;
    cat_tx((void *)1, "FA;FB;");
    cat_tx((void *)1, "TX1;");
    handle_cat_packet("FA014074000;");   // not logged by handle_cat_packet (rx_cb logs), so log it here
    size_t n = cat_log_read(mark, le, 200);
    CHECK(n == 3 && strcmp(le[0].text, "FA;") == 0 && le[0].kind == CAT_LOG_CMD && le[0].dir == 'T' &&
          strcmp(le[1].text, "FB;") == 0 && strcmp(le[2].text, "TX1;") == 0 && le[2].kind == CAT_LOG_BLOCKED,
          "log entries %zu", n);
    CHECK(strstr(g_sent, "TX1") == NULL, "blocked command reached the port: %s", g_sent);
    for (int i = 0; i < 300; i++) cat_tx((void *)1, "FA;");
    n = cat_log_read(0, le, 200);
    CHECK(n == 160 && le[159].seq == le[0].seq + 159, "log ring keeps 160, got %zu", n);
    n = cat_log_read(le[159].seq, le, 200);
    CHECK(n == 0, "nothing newer");

    n = cat_log_read(UINT32_MAX, le, 200);
    CHECK(n == 160, "resume log after stale browser cursor");

    const uint32_t through_ms = (uint32_t)(g_now_us / 1000);
    log_cmds('T', CAT_LOG_POLL, "RM5;RM6;RM1;");
    log_cmds('R', CAT_LOG_POLL, "RM5005000;RM6026000;");
    g_now_us += 1000;
    log_cmds('T', CAT_LOG_ERROR, "RM5;");
    n = cat_log_recent_tx_meters(through_ms, le, 3);
    CHECK(n == 3 && strcmp(le[0].text, "RM6026000;") == 0 &&
          strcmp(le[1].text, "RM5005000;") == 0 && strcmp(le[2].text, "RM6;") == 0,
          "meter diagnostics select newest matching RX and TX entries before the cutoff");
    n = cat_log_recent_tx_meters(through_ms + 1, le, 1);
    CHECK(n == 1 && le[0].kind == CAT_LOG_ERROR && strcmp(le[0].text, "RM5;") == 0,
          "meter diagnostics include failed writes");

    for (unsigned i = 0; i < 2; ++i) {
        use(i ? "0761" : "0840");
        s_state.tx_cat = true;
        g_now_us += 1000;
        g_sent[0] = '\0';
        keyed_poll_t poll = {0};
        poll_keyed((void *)1, s_rig, &poll);
        CHECK(strcmp(g_sent, i ? "TX;RI0;FT;MD0;" : "RI0;TX;FT;MD0;MD1;") == 0,
              "only TX status is outstanding on %s", s_rig->name);
        CHECK(g_write_timeout_ms == (i ? 300 : 1200), "keyed poll uses the model-specific write budget");
        CHECK(cat_enqueue("FA;") == ESP_ERR_INVALID_STATE, "ordinary commands cannot queue during TX");
        CHECK(cat_tx((void *)1, "FA;") == ESP_ERR_INVALID_STATE, "ordinary writes cannot reach USB during TX");
        g_sent[0] = '\0';
        g_now_us += POLL_PERIOD_MS * 1000;
        handle_cat_packet("TX1;");
        handle_cat_packet(i ? "RI00;" : "RI00010000;");
        handle_cat_packet("FT0;");
        if (i) {
            poll_keyed((void *)1, s_rig, &poll);
            CHECK(!g_sent[0], "FTDX10 must wait for MD0");
        }
        handle_cat_packet("MD01;");
        if (!i) {
            poll_keyed((void *)1, s_rig, &poll);
            CHECK(!g_sent[0], "FTX-1 must wait for MD1");
            handle_cat_packet("MD11;");
        }
        g_write_duration_us = i ? 125000 : 950000;
        const cat_write_stats_t before_slow_write = cat_get_write_stats();
        poll_keyed((void *)1, s_rig, &poll);
        g_write_duration_us = 0;
        CHECK(strcmp(g_sent, "RM5;RM6;") == 0, "complete status releases meter poll");
        CHECK(cat_get_write_stats().keyed.over_budget == before_slow_write.keyed.over_budget,
              "model latency is inside the keyed write budget");
        g_sent[0] = '\0';
        handle_cat_packet("RM6046000;");
        poll_keyed((void *)1, s_rig, &poll);
        CHECK(!g_sent[0], "unsolicited RM6 must not release a poll without RM5");
        const int64_t reply_timeout = i ? FTDX10_KEYED_REPLY_TIMEOUT_US : FTX1_KEYED_REPLY_TIMEOUT_US;
        g_now_us = poll.sent_at + reply_timeout - 1;
        poll_keyed((void *)1, s_rig, &poll);
        CHECK(!g_sent[0], "partial meter poll waits for the model-specific deadline");
        ++g_now_us;
        poll_keyed((void *)1, s_rig, &poll);
        CHECK(g_sent[0], "next batch allowed at the deadline without a complete reply");
        s_unkey_requested = true;
        g_sent[0] = '\0';
        g_now_us += reply_timeout;
        poll_keyed((void *)1, s_rig, &poll);
        CHECK(!g_sent[0], "no new poll while unkeying");
        s_ptt_lock = (void *)1;
        s_ptt_port = (void *)1;
        CHECK(cat_ptt(false) == ESP_OK && strcmp(g_sent, "TX0;") == 0 &&
              g_write_timeout_ms == 200, "TX0 keeps its independent short budget");
    }

    use("0761");
    s_state.tx_cat = true;
    g_now_us += 1000;
    keyed_poll_t startup_poll = {0};
    g_sent[0] = '\0';
    poll_keyed((void *)1, s_rig, &startup_poll);
    ++g_now_us;
    handle_cat_packet("TX1;");
    handle_cat_packet("RI01;");
    handle_cat_packet("FT0;");
    handle_cat_packet("MD0C;");
    g_now_us = startup_poll.sent_at + POLL_PERIOD_MS * 1000;
    g_sent[0] = '\0';
    poll_keyed((void *)1, s_rig, &startup_poll);
    CHECK(strcmp(g_sent, "TX;RI0;FT;MD0;") == 0, "early high SWR gets an expedited status recheck");
    s_state.tx_cat = false;

    // Exercise a full minute of FTDX10 polling without a single MD1 reply.
    use("0761");
    s_state.tx_cat = true;
    s_mode_status_us[1] = 0;
    g_now_us += 1000;
    const int64_t trial_started = g_now_us;
    keyed_poll_t trial_poll = {0};
    tx_swr_guard_t trial_guard = {.rig = CAT_RIG_FTDX10};
    unsigned meter_polls = 0;
    for (unsigned i = 0; i < 600; ++i) {
        g_sent[0] = '\0';
        poll_keyed((void *)1, s_rig, &trial_poll);
        g_now_us += 10000;
        if (strcmp(g_sent, "TX;RI0;FT;MD0;") == 0) {
            handle_cat_packet("TX1;");
            handle_cat_packet("RI00;");
            handle_cat_packet("FT0;");
            handle_cat_packet("MD0C;");
        } else if (strcmp(g_sent, "RM5;RM6;") == 0) {
            ++meter_polls;
            handle_cat_packet("RM5033000;");
            ++g_now_us;
            handle_cat_packet("RM6000000;");
        }
        g_now_us = trial_started + (i + 1) * 100000LL;
        const cat_tx_meters_t trial_meters = cat_get_tx_meters();
        CHECK(tx_swr_check(&trial_guard, &trial_meters, trial_started, g_now_us, 300) == TX_SWR_OK,
              "FTDX10 meter safety remains satisfied without MD1 at iteration %u", i);
    }
    CHECK(meter_polls == 400, "status releases the next meter poll on time: %u", meter_polls);
    CHECK(cat_ptt_mode_error(trial_started) == NULL, "FTDX10 MAIN mode is confirmed without MD1");
    handle_cat_packet("FT1;");
    handle_cat_packet("MD1C;");
    CHECK(strcmp(cat_ptt_mode_error(trial_started), "Select MAIN for remote PTT") == 0,
          "FTDX10 remote TX does not use an unmonitored SUB mode");
    s_state.tx_cat = false;

    // FTDX10 regression: a lost meter pair and then a partial status batch
    // must leave time to collect fresh meters before the 1.1 s watchdog.
    use("0761");
    s_state.tx_cat = true;
    g_now_us += 1000;
    const int64_t last_ftdx10_pair = g_now_us;
    handle_cat_packet("RM5033000;");
    ++g_now_us;
    handle_cat_packet("RM6000000;");
    g_now_us += POLL_PERIOD_MS * 1000;
    keyed_poll_t ftdx10_poll = {.sent_at = g_now_us};
    g_sent[0] = '\0';
    g_now_us += FTDX10_KEYED_REPLY_TIMEOUT_US;
    poll_keyed((void *)1, s_rig, &ftdx10_poll);
    handle_cat_packet("TX1;");
    handle_cat_packet("RI00;");
    CHECK(strcmp(g_sent, "TX;RI0;FT;MD0;") == 0, "missing meter pair advances to status");
    g_sent[0] = '\0';
    g_now_us += FTDX10_KEYED_REPLY_TIMEOUT_US;
    poll_keyed((void *)1, s_rig, &ftdx10_poll);
    CHECK(strcmp(g_sent, "RM5;RM6;") == 0, "partial status does not starve meters");
    g_now_us += 125000;
    handle_cat_packet("RM5033000;");
    ++g_now_us;
    handle_cat_packet("RM6000000;");
    CHECK(s_tx_meters.swr_po_us > last_ftdx10_pair && s_tx_meters.swr_us == g_now_us &&
          g_now_us - last_ftdx10_pair < 1100000, "fresh PO/SWR pair arrives before the previous pair expires");
    s_state.tx_cat = false;

    use("0840");
    g_now_us += KEYED_CAT_MAX_AGE_US;
    const int64_t keyed_at = g_now_us;
    handle_cat_packet("TX1;");
    handle_cat_packet("RI00010000;");
    s_state.tx_cat = true;
    g_write_result = ESP_ERR_TIMEOUT;
    g_write_duration_us = 1200000;
    keyed_poll_t poll = {0};
    const unsigned timeouts = cat_get_write_stats().keyed.timeouts;
    poll_keyed((void *)1, s_rig, &poll);
    g_write_result = ESP_OK;
    g_write_duration_us = 0;
    CHECK(cat_get_write_stats().keyed.timeouts == timeouts + 1, "write timeout is counted");
    CHECK(cat_ptt_fault(keyed_at, 1500000) == NULL, "one write timeout does not lose CAT");
    g_now_us = keyed_at + KEYED_CAT_MAX_AGE_US - 1;
    CHECK(cat_ptt_fault(keyed_at, 1500000) == NULL, "wait full three seconds for responses");
    ++g_now_us;
    handle_cat_packet("RM6256000;");
    CHECK(strcmp(cat_ptt_fault(keyed_at, 1500000), "CAT lost") == 0,
          "malformed meter cannot refresh CAT liveness");
    handle_cat_packet("RM6046000;");
    CHECK(strcmp(cat_ptt_fault(keyed_at, 1500000), "Radio TX status unavailable") == 0,
          "a valid meter keeps CAT alive, but cannot replace stale TX confirmations");
    handle_cat_packet("RI01010000;");
    handle_cat_packet("RI00010000;");
    CHECK(strcmp(cat_ptt_fault(keyed_at, 1500000), "High SWR") == 0,
          "a Hi-SWR report is retained even if normal status arrives before the watchdog");
    s_state.tx_cat = false;

    use("0840");
    handle_cat_packet("FT0;");
    handle_cat_packet("MD02;");
    handle_cat_packet("EX0101130;");
    CHECK(cat_voice_mode_error(true) != NULL, "FTX-1 MIC source refuses browser voice");
    handle_cat_packet("EX0101131;");
    CHECK(cat_voice_mode_error(true) == NULL, "FTX-1 USB source permits browser voice");
    handle_cat_packet("EX0101132;");
    CHECK(cat_voice_mode_error(true) != NULL, "FTX-1 Bluetooth refuses browser voice");
    handle_cat_packet("EX0101133;");
    CHECK(cat_voice_mode_error(true) == NULL, "FTX-1 AUTO selects USB for CAT voice");
    g_now_us += MOD_MENU_MAX_AGE_US + 1;
    CHECK(cat_voice_mode_error(true) != NULL, "stale FTX-1 AUTO cannot authorize voice");
    CHECK(ok("EX010113;") && !ok("EX0101131;") && !ok("EX0101133;"), "FTX-1 SSB source is read only");
    use("0761");
    s_ssb_menu_us[0] = s_ssb_menu_us[1] = 0;
    handle_cat_packet("EX0101131;");
    CHECK(cat_voice_mode_error(true) != NULL, "FTDX10 rear select must be confirmed");
    handle_cat_packet("EX0101140;");
    CHECK(cat_voice_mode_error(true) != NULL, "FTDX10 REAR DATA source refuses browser voice");
    handle_cat_packet("EX0101141;");
    CHECK(cat_voice_mode_error(true) == NULL, "FTDX10 REAR USB permits browser voice");
    CHECK(ok("EX010114;") && !ok("EX0101141;"), "FTDX10 REAR SELECT is read only");
    handle_cat_packet("MD0C;");
    CHECK(cat_voice_mode_error(false) != NULL, "voice stops if radio leaves SSB");
    use("0840");
    handle_cat_packet("EX0104130;");
    CHECK(cat_data_mode_error() != NULL, "FTX-1 DATA MIC cannot authorize FT8");
    handle_cat_packet("EX0104133;");
    CHECK(cat_data_mode_error() != NULL, "FTX-1 DATA AUTO is not confirmed USB");
    handle_cat_packet("EX0104131;");
    CHECK(cat_data_mode_error() == NULL, "FTX-1 DATA USB authorizes FT8");
    CHECK(ok("EX010413;") && !ok("EX0104131;"), "FTX-1 DATA MOD SOURCE read only");
    g_now_us += MOD_MENU_MAX_AGE_US + 1;
    CHECK(cat_data_mode_error() != NULL, "stale DATA USB readback cannot authorize FT8");
    use("0761");
    handle_cat_packet("EX0104151;");
    CHECK(cat_data_mode_error() != NULL, "FTDX10 requires fresh DATA REAR SELECT");
    handle_cat_packet("EX0104160;");
    CHECK(cat_data_mode_error() != NULL, "FTDX10 DATA REAR jack cannot authorize FT8");
    handle_cat_packet("EX0104161;");
    CHECK(cat_data_mode_error() == NULL, "FTDX10 DATA REAR USB authorizes FT8");
    handle_cat_packet("EX0104150;");
    CHECK(cat_data_mode_error() != NULL, "FTDX10 DATA MIC cannot authorize FT8");
    CHECK(cat_cmds_allowed("EX010113;EX010114;EX010415;EX010416;", CAT_TX_MAX_CMDS) && !ok("EX0104161;"), "FTDX10 DATA settings read only");
    test_startup();
    test_rejected_tx_read_after_unkey();
    test_hot_swap();
    test_slow_radio_power_on();
    test_rigctl_cat_writes();
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
