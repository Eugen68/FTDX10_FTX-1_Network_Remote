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
#define TEST_CAT_ADVANCE_TIME
int64_t g_now_us = 1000000;
#include "cat.c"   // the real file, built against the stubs in stub/
#include "tx_swr.h"
uint32_t g_write_timeout_ms;
int64_t g_write_duration_us;
esp_err_t g_write_result;

static bool tx_enabled, tx_password;
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

static void inject(const char *reply)
{
    rx_cb((const uint8_t *)reply, strlen(reply), NULL);
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
    if (len == 3 && memcmp(cmd, "ID;", len) == 0) {
        inject("IDABCD;ID123;FA007130000;ID0840;");
    } else if (len == 3 && memcmp(cmd, "AI;", len) == 0) {
        inject(startup_ai);
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

static void test_startup(void)
{
    s_disconnected = false;
    g_write_result = ESP_OK;
    s_state.tx_cat = false;
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
    cat_new_usb_device((void *)2);
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
    cat_new_usb_device((void *)2);
    g_now_us += CAT_SETTLE_US - 1;
    CHECK(cat_tx((void *)1, "ID;") == ESP_ERR_INVALID_STATE && !g_sent[0], "new settling period after reset");
    ++g_now_us;

    use("0840");
    s_rejected_count = 0;
    reject_notch = true;
    const unsigned rejected = cat_rejected_commands();
    initialize_reads((void *)1, s_rig);
    CHECK(strcmp(g_sent, "FA;FB;MD0;MD1;AI;AG0;AG1;SQ0;SQ1;RI0;TX;RM0;BP00;BP01;BP10;BP11;FT;AI1;") == 0,
          "FTX-1 reads before first write");
    CHECK(cat_rejected_commands() == rejected + 1 && poll_rejected("BP00;"), "count and attribute explicit rejection");
    g_sent[0] = '\0';
    send_poll((void *)1, "BP00;BP01;");
    CHECK(strcmp(g_sent, "BP01;") == 0, "skip only rejected periodic command");

    use("0761");
    s_rejected_count = 0;
    reject_notch = false;
    startup_ai = "AI1;";
    g_sent[0] = '\0';
    initialize_reads((void *)1, s_rig);
    CHECK(strcmp(g_sent, "FA;FB;MD0;MD1;AI;AG0;SQ0;BY;TX;RM1;BP00;BP01;FT;") == 0, "FTDX10 leaves AI1 alone");
    startup_ai = "?;";
    g_sent[0] = '\0';
    initialize_reads((void *)1, s_rig);
    CHECK(strstr(g_sent, "AI;") && !strstr(g_sent, "AI1;"), "rejected AI never enables it");
    g_cdc_send_hook = NULL;
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
    const char *never[] = {"TX1;", "TX2;", "MX1;", "KY0;", "VX1;", "PC100;", "FT0;", "FT1;", "FT2;", "FT3;", "EX010411100;x", "AI0;", NULL};
    const char *ids[] = {"0840", "0761", "9999", NULL};
    for (int r = 0; ids[r]; r++) { use(ids[r]); for (int k = 0; never[k]; k++) CHECK(!ok(never[k]), "%s allowed %s", ids[r], never[k]); }


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
    g_now_us += VOICE_MENU_MAX_AGE_US + 1;
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
    test_startup();
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
