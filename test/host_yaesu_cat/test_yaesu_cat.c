/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "yaesu_cat_protocol.h"

static cat_state_t ftdx10 = {
    .connected = true, .rig = CAT_RIG_FTDX10, .rig_id = "0761", .can_control = true,
    .freq_min = 30000, .freq_max = 75000000, .freq_main = 14074000, .freq_sub = 7074000,
    .mode_main_raw = 'C', .mode_sub_raw = '3', .tx_vfo = CAT_TX_VFO_MAIN,
    .rx_vfo_known = true, .rx_vfo_sub = false, .s_main = 120, .width_code = {13, -1}, .key_pitch = 30,
};
static cat_state_t ftx1 = {
    .connected = true, .rig = CAT_RIG_FTX1, .rig_id = "0840", .can_control = true, .dual_rx = true,
    .c4fm = true, .freq_min = 30000, .freq_max = 470000000, .freq_main = 145500000, .freq_sub = 21074000,
    .mode_main_raw = 'H', .mode_sub_raw = '2', .tx_vfo = CAT_TX_VFO_SUB,
    .rx_vfo_known = true, .rx_vfo_sub = true, .s_main = 7, .s_sub = 255, .width_code = {9, 13},
    .key_pitch = -1,
};

static unsigned applied;
static yaesu_set_t last;
static bool accept_writes = true;

static bool apply(const yaesu_set_t *set, void *context)
{
    (void)context;
    ++applied;
    last = *set;
    return accept_writes;
}

static yaesu_reply_t run(const cat_state_t *cat, yaesu_session_t *session, const char *cmd, char *reply)
{
    return yaesu_cat_command(cmd, cat, session, apply, NULL, reply);
}

static void read_is(const cat_state_t *cat, const char *cmd, const char *expected)
{
    yaesu_session_t session = {0};
    char reply[YAESU_CAT_REPLY_MAX];
    const yaesu_reply_t r = run(cat, &session, cmd, reply);
    if (strcmp(reply, expected) != 0) {
        fprintf(stderr, "%s -> '%s', expected '%s'\n", cmd, reply, expected);
    }
    assert(strcmp(reply, expected) == 0);
    assert(r.length == strlen(expected));
    assert(!r.write);
}

static void write_is(const cat_state_t *cat, const char *cmd, bool applied_ok, yaesu_action_t action,
                     bool sub, uint32_t value)
{
    yaesu_session_t session = {0};
    char reply[YAESU_CAT_REPLY_MAX];
    const unsigned before = applied;
    const yaesu_reply_t r = run(cat, &session, cmd, reply);
    assert(r.write && r.length == 0 && reply[0] == '\0');
    assert(r.rejected == !applied_ok);
    if (applied_ok) {
        assert(applied == before + 1);
        assert(last.action == action && last.sub == sub && last.value == value);
    }
}

static void refused_silently(const cat_state_t *cat, const char *cmd)
{
    yaesu_session_t session = {0};
    char reply[YAESU_CAT_REPLY_MAX];
    const unsigned before = applied;
    const yaesu_reply_t r = run(cat, &session, cmd, reply);
    assert(r.write && r.rejected && r.length == 0 && applied == before);
}

static void test_reads(void)
{
    read_is(&ftdx10, "ID", "ID0761;");
    read_is(&ftdx10, "FA", "FA014074000;");
    read_is(&ftdx10, "fb", "FB007074000;");
    // FTDX10 IF / OI: 28 bytes with a 3-digit memory field.
    read_is(&ftdx10, "IF", "IF001014074000+000000C00000;");
    read_is(&ftdx10, "OI", "OI001007074000+000000300000;");
    assert(strlen("IF001014074000+000000C00000;") == 28);
    read_is(&ftdx10, "MD0", "MD0C;");
    read_is(&ftdx10, "MD1", "MD13;");
    read_is(&ftdx10, "FT", "FT0;");
    read_is(&ftdx10, "ST", "ST0;");
    read_is(&ftdx10, "VS", "VS0;");
    read_is(&ftdx10, "TX", "TX0;");
    read_is(&ftdx10, "PS", "PS1;");
    read_is(&ftdx10, "SM0", "SM0120;");
    read_is(&ftdx10, "SM1", "?;");
    read_is(&ftdx10, "SH0", "SH0013;");
    read_is(&ftdx10, "FR", "?;");
    // CW pitch: OmniRig polls KP; in every cycle and reads 5 bytes.
    read_is(&ftdx10, "KP", "KP30;");
    cat_state_t pitch = ftdx10;
    pitch.key_pitch = 45;
    read_is(&pitch, "KP", "KP45;");
    read_is(&ftx1, "KP", "KP30;");            // not read yet: 600 Hz, never ?;
    refused_silently(&ftdx10, "KP40");        // the pitch is never changed from the network

    // FTX-1 IF / OI: 30 bytes with a 5-digit memory field and the C4FM modes.
    read_is(&ftx1, "IF", "IF00000145500000+000000H00000;");
    assert(strlen("IF00000145500000+000000H00000;") == 30);
    read_is(&ftx1, "OI", "OI00000021074000+000000200000;");
    read_is(&ftx1, "FT", "FT1;");
    read_is(&ftx1, "ST", "ST0;");            // receives and transmits on SUB: no split
    read_is(&ftx1, "VS", "VS1;");
    read_is(&ftx1, "SM1", "SM1255;");
    read_is(&ftx1, "SH1", "SH1013;");

    cat_state_t keyed = ftdx10;
    keyed.tx_cat = true;
    read_is(&keyed, "TX", "TX1;");
    keyed.tx_cat = false;
    keyed.tx_radio = true;
    read_is(&keyed, "TX", "TX2;");
}

static void test_writes(void)
{
    write_is(&ftdx10, "FA007025000", true, YAESU_SET_FREQ, false, 7025000);
    write_is(&ftdx10, "FB014074000", true, YAESU_SET_FREQ, true, 14074000);
    write_is(&ftdx10, "FA000010000", false, 0, false, 0);   // below the radio's range
    write_is(&ftdx10, "FA076000000", false, 0, false, 0);   // above it
    write_is(&ftdx10, "MD03", true, YAESU_SET_MODE, false, '3');
    write_is(&ftdx10, "MD0H", false, 0, false, 0);          // no C4FM on the FTDX10
    write_is(&ftx1, "MD1I", true, YAESU_SET_MODE, true, 'I');
    write_is(&ftdx10, "VS1", true, YAESU_SELECT_VFO, true, 0);
    write_is(&ftdx10, "FT3", true, YAESU_SET_TX_VFO, true, 0);
    write_is(&ftdx10, "FT1", false, 0, false, 0);            // FTDX10 sets with 2 / 3 only
    write_is(&ftx1, "FT0", true, YAESU_SET_TX_VFO, false, 0);
    // OmniRig's split on / off: transmit on the other VFO, or on the receiving one.
    write_is(&ftdx10, "ST1", true, YAESU_SET_TX_VFO, true, 0);
    write_is(&ftdx10, "ST0", true, YAESU_SET_TX_VFO, false, 0);
    write_is(&ftx1, "ST1", true, YAESU_SET_TX_VFO, false, 0);
    write_is(&ftx1, "ST0", true, YAESU_SET_TX_VFO, true, 0);
    write_is(&ftdx10, "ST2", false, 0, false, 0);            // split +5 kHz is not offered
    {
        cat_state_t split = ftdx10;
        split.tx_vfo = CAT_TX_VFO_SUB;
        read_is(&split, "ST", "ST1;");
    }
    write_is(&ftdx10, "SV", true, YAESU_SWAP_VFO, false, 0);
    write_is(&ftdx10, "AB", true, YAESU_COPY_A_TO_B, false, 0);
    write_is(&ftdx10, "BA", true, YAESU_COPY_B_TO_A, false, 0);

    // The board refused it: still no reply on the wire.
    accept_writes = false;
    write_is(&ftdx10, "FA007030000", false, 0, false, 0);
    accept_writes = true;

    // Never forwarded, never answered.
    refused_silently(&ftdx10, "TX1");
    refused_silently(&ftdx10, "TX2");
    refused_silently(&ftdx10, "PS0");
    refused_silently(&ftdx10, "EX010413050");
    refused_silently(&ftdx10, "KY TEST");

    yaesu_session_t session = {0};
    char reply[YAESU_CAT_REPLY_MAX];
    const unsigned before = applied;
    yaesu_reply_t r = run(&ftdx10, &session, "TX0", reply);
    assert(r.write && !r.rejected && !r.length && applied == before);

    cat_state_t read_only = ftdx10;
    read_only.can_control = false;
    refused_silently(&read_only, "FA007025000");
}

static void test_auto_information(void)
{
    yaesu_session_t session = {0};
    char reply[YAESU_CAT_REPLY_MAX];
    yaesu_reply_t r = run(&ftdx10, &session, "AI1", reply);
    assert(r.write && !r.rejected && session.ai);
    read_is(&ftdx10, "AI", "AI0;");   // a fresh session
    r = run(&ftdx10, &session, "AI", reply);
    assert(strcmp(reply, "AI1;") == 0);
    r = run(&ftdx10, &session, "AI0", reply);
    assert(!session.ai);

    cat_state_t now = ftdx10;
    now.freq_main = 14075000;
    now.mode_sub_raw = '2';
    char changes[96];
    const size_t n = yaesu_cat_changes(&ftdx10, &now, changes, sizeof(changes));
    assert(n == strlen("FA014075000;MD12;"));
    assert(strcmp(changes, "FA014075000;MD12;") == 0);
    assert(yaesu_cat_changes(&ftdx10, &ftdx10, changes, sizeof(changes)) == 0);
}

static void test_offline(void)
{
    cat_state_t off = ftdx10;
    off.connected = false;
    yaesu_session_t session = {0};
    char reply[YAESU_CAT_REPLY_MAX];
    yaesu_reply_t r = run(&off, &session, "FA", reply);
    assert(r.length == 0 && reply[0] == '\0');
    r = run(&off, &session, "FA007025000", reply);
    assert(r.length == 0);

    cat_state_t unidentified = ftdx10;
    unidentified.rig_id[0] = '\0';
    r = run(&unidentified, &session, "ID", reply);
    assert(r.length == 0);
}

int main(void)
{
    test_reads();
    test_writes();
    test_auto_information();
    test_offline();
    puts("yaesu cat protocol: ok");
    return 0;
}
