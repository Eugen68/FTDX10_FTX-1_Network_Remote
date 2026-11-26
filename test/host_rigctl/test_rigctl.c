/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rigctl_protocol.h"

static cat_state_t cat = {
    .rig = CAT_RIG_FTDX10, .connected = true, .can_control = true,
    .rx_vfo_known = true, .tx_radio_known = true, .tx_vfo = CAT_TX_VFO_SUB,
    .freq_min = 30000, .freq_max = 75000000, .freq_main = 7133000, .freq_sub = 14074000,
    .s_main = 120, .s_sub = 255, .mode_main_raw = '1', .mode_sub_raw = 'C', .width_code = {13, 13},
};
static unsigned updates;
static rigctl_set_t last;

static bool apply(const rigctl_set_t *set, void *context)
{
    assert(context == &cat);
    ++updates;
    last = *set;
    return true;
}

static void check(const char *line, const char *expected, bool rejected, bool close)
{
    char input[RIGCTL_LINE_SIZE];
    char reply[RIGCTL_REPLY_SIZE] = "";
    strcpy(input, line);
    const rigctl_reply_t result = rigctl_command(input, &cat, apply, &cat, reply);
    assert(result.length == strlen(expected));
    assert(strcmp(reply, expected) == 0);
    assert(result.rejected == rejected && result.close == close);
}

int main(void)
{
    const char *const names[] = {"LSB", "USB", "CW", "FM", "AM", "RTTY", "CWR",
                                "PKTLSB", "RTTYR", "PKTFM", "PKTUSB", "FMN", "AMN", "PKTFMN"};
    const char codes[] = "123456789ACBDF";
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(rigctl_mode_code(names[i]) == codes[i]);
        assert(strcmp(rigctl_mode_name(codes[i]), names[i]) == 0);
    }
    assert(!rigctl_mode_code("C4FM") && !rigctl_mode_name('H'));
    assert(rigctl_width_hz(CAT_RIG_FTDX10, '2', 13) == 2400);
    assert(rigctl_width_code(CAT_RIG_FTDX10, '2', 2400) == 13);
    assert(rigctl_width_code(CAT_RIG_FTX1, 'C', 2400) == 17);
    assert(rigctl_width_hz(CAT_RIG_FTX1, '5', -1) == 6000);
    assert(rigctl_width_hz(CAT_RIG_FTDX10, '5', -1) == 9000);
    assert(rigctl_width_hz(CAT_RIG_FTX1, 'B', -1) == 9000);
    assert(rigctl_width_hz(CAT_RIG_FTX1, '4', -1) == 16000);
    assert(rigctl_width_code(CAT_RIG_FTDX10, '2', 2300) < 0);
    assert(rigctl_width_hz(CAT_RIG_FTDX10, '2', -1) == 0);

    check("\\chk_vfo", "0\n", false, false);
    check("+\\chk_vfo", "ChkVFO: 0\nRPRT 0\n", false, false);
    char dump[] = "\\dump_state", reply[RIGCTL_REPLY_SIZE];
    assert(!rigctl_command(dump, &cat, apply, &cat, reply).rejected);
    assert(strncmp(reply, "1\n1042\n0\n30000 75000000 ", strlen("1\n1042\n0\n30000 75000000 ")) == 0);
    assert(strstr(reply, "ptt_type=0x1\n") && strstr(reply, "\nrig_model=1042\ndone\n"));
    check("f\r", "7133000\n", false, false);
    check("+\\get_freq", "get_freq:\nFrequency: 7133000\nRPRT 0\n", false, false);
    check("m", "LSB\n2400\n", false, false);
    check("+\\get_vfo_info VFOB", "get_vfo_info: VFOB\nFreq: 14074000\nMode: PKTUSB\nWidth: 1200\nSplit: 1\nSatMode: 0\nRPRT 0\n", false, false);
    check("+s", "get_split_vfo:\nSplit: 1\nTX VFO: VFOB\nRPRT 0\n", false, false);
    check("v", "VFOA\n", false, false);
    check("t", "0\n", false, false);
    check("\\get_powerstat", "1\n", false, false);
    check("f VFOB", "14074000\n", false, false);
    check("+m VFOB", "get_mode: VFOB\nMode: PKTUSB\nPassband: 1200\nRPRT 0\n", false, false);
    check("\\get_freq Main", "7133000\n", false, false);
    check("t VFOA", "0\n", false, false);
    check("f VFOC", "RPRT -1\n", true, false);
    check("\\get_lock_mode", "0\n", false, false);
    check("l STRENGTH", "0\n", false, false);
    check("\\get_level VFOA RAWSTR", "120\n", false, false);
    check("l VFOB STRENGTH", "RPRT -11\n", true, false);
    check("l RFPOWER", "RPRT -11\n", true, false);
    check("j VFOA", "RPRT -11\n", true, false);
    check("\\get_ant 1", "RPRT -11\n", true, false);
    check("\\dump_caps", "RPRT -11\n", true, false);
    check("\\get_vfo_list", "VFOA VFOB\n", false, false);
    check("\\get_rig_info",
          "VFO=VFOA Freq=7133000 Mode=LSB Width=2400 RX=1 TX=0\n"
          "VFO=VFOB Freq=14074000 Mode=PKTUSB Width=1200 RX=0 TX=1\n"
          "Split=1 SatMode=0\n"
          "Rig=FTDX10\n"
          "App=Network Remote\n"
          "Version=20241103 1.1.0\n"
          "Model=1042\n"
          "CRC=0x80f781ce\n"
          "\n", false, false);
    check("+\\get_rig_info",
          "get_rig_info:\n"
          "VFO=VFOA Freq=7133000 Mode=LSB Width=2400 RX=1 TX=0\n"
          "VFO=VFOB Freq=14074000 Mode=PKTUSB Width=1200 RX=0 TX=1\n"
          "Split=1 SatMode=0\n"
          "Rig=FTDX10\n"
          "App=Network Remote\n"
          "Version=20241103 1.1.0\n"
          "Model=1042\n"
          "CRC=0x80f781ce\n"
          "\n"
          "RPRT 0\n", false, false);
    assert(updates == 0);

    check("+F 14074000.000000", "set_freq: 14074000.000000\nRPRT 0\n", false, false);
    assert(last.action == RIGCTL_FREQ && !last.sub && last.value == 14074000);
    check("M PKTUSB 2400", "RPRT 0\n", false, false);
    assert(last.action == RIGCTL_MODE && last.value == 'C' && last.width == 2400);
    check("V VFOB", "RPRT 0\n", false, false);
    assert(last.action == RIGCTL_VFO && last.sub);
    check("S 1 VFOB", "RPRT 0\n", false, false);
    assert(last.action == RIGCTL_SPLIT && last.sub);
    check("S 0 VFOB", "RPRT 0\n", false, false);
    assert(last.action == RIGCTL_SPLIT && !last.sub);

    check("F 7133000.1", "RPRT -1\n", true, false);
    check("F 999", "RPRT -1\n", true, false);
    check("M USB", "RPRT -1\n", true, false);
    check("M USB 2400 extra", "RPRT -1\n", true, false);
    check("F 7133000;TX1;", "RPRT -1\n", true, false);
    check("garbage", "RPRT -11\n", true, false);
    check("T 1", "RPRT -11\n", true, false);
    check("T 0", "RPRT -11\n", true, false);
    check("\\set_ptt 1", "RPRT -11\n", true, false);
    cat.tx_radio = cat.tx = true;
    check("t", "1\n", false, false);
    check("F 7100000", "RPRT -11\n", true, false);
    check("M AM 0", "RPRT -11\n", true, false);
    check("V VFOB", "RPRT -11\n", true, false);
    check("S 1 VFOB", "RPRT -11\n", true, false);
    cat.tx_radio = cat.tx = cat.tx_radio_known = false;
    check("F 7100000", "RPRT -11\n", true, false);
    assert(updates == 5);

    cat.rig = CAT_RIG_FTX1;
    cat.dual_rx = true;
    check("l Sub STRENGTH", "60\n", false, false);
    cat.rx_vfo_sub = true;
    check("v", "Sub\n", false, false);
    check("s", "0\nSub\n", false, false);
    check("m", "PKTUSB\n1200\n", false, false);
    check("f", "14074000\n", false, false);
    cat.connected = false;
    check("\\get_powerstat", "0\n", false, false);
    check("f", "RPRT -11\n", true, false);
    check("q", "RPRT 0\n", false, true);
    puts("NET rigctl parser, mode and width maps, and TX restrictions passed.");
}
