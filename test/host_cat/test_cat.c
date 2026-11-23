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
int64_t g_now_us = 1000000;
#include "cat.c"   // the real file, built against the stubs in stub/

static bool tx_enabled, tx_password;
bool app_config_tx_enabled(void) { return tx_enabled && tx_password; }

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool ok(const char *c) { return cat_cmds_allowed(c, 1); }

static void use(const char *id) {
    memset(s_rig_id, 0, sizeof s_rig_id);
    if (id) { memcpy(s_rig_id, id, 4); s_rig = profile_for_id(id); } else s_rig = &s_profile_none;
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
    handle_cat_packet("RI01;");
    CHECK(strcmp(cat_ptt_fault(g_now_us, 1500000), "High SWR") == 0, "FTDX10 RI reports high SWR");
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
        CHECK(cat_ptt_mode_error() == NULL, "MAIN mode %c permits PTT", permitted[i]);
    }
    for (size_t i = 0; i < sizeof(refused) - 1; i++) {
        mode_packet[3] = refused[i];
        handle_cat_packet(mode_packet);
        CHECK(cat_ptt_mode_error() != NULL, "MAIN mode %c must block PTT", refused[i]);
    }
    handle_cat_packet("MD02;");
    handle_cat_packet("FT1;");
    handle_cat_packet("MD14;");
    CHECK(cat_ptt_mode_error() != NULL, "SUB FM must block PTT when SUB was selected for TX");
    handle_cat_packet("MD18;");
    CHECK(cat_ptt_mode_error() == NULL, "SUB DATA-LSB is permitted");
    g_now_us += RADIO_STATUS_MAX_AGE_US + 1;
    CHECK(cat_ptt_mode_error() != NULL, "stale mode must block PTT");
    handle_cat_packet("MD02;");
    CHECK(cat_ptt_mode_error() != NULL, "unconfirmed TX VFO must block PTT");
    handle_cat_packet("FT0;");
    CHECK(cat_set_mode_char(false, '5') == ESP_OK, "queue an AM mode change");
    CHECK(cat_ptt_mode_error() != NULL, "optimistic mode change cannot authorize PTT");

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
    handle_cat_packet("RM6089000;");
    meters = cat_get_tx_meters();
    CHECK(meters.rig == CAT_RIG_FTX1 && meters.swr == 89, "FTX-1 SWR");
    use("9999");
    CHECK(!ok("RM5;") && !ok("RM6;"), "unknown radio blocks transmit meters");

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

    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
