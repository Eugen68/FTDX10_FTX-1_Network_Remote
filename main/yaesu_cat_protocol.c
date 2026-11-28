/* SPDX-License-Identifier: MIT */
#include "yaesu_cat_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static yaesu_reply_t text(char *reply, const char *value)
{
    const size_t n = strlen(value);
    memcpy(reply, value, n + 1);
    return (yaesu_reply_t){.length = n};
}

static yaesu_reply_t rejected_read(char *reply)
{
    yaesu_reply_t r = text(reply, "?;");
    r.rejected = true;
    return r;
}

/* Writes never answer. A client that reads fixed-length replies would otherwise lose step. */
static yaesu_reply_t write_result(char *reply, bool applied)
{
    reply[0] = '\0';
    return (yaesu_reply_t){.rejected = !applied, .write = true};
}

static bool digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return s[n] == '\0';
}

static bool valid_mode(const cat_state_t *cat, char mode)
{
    if ((mode >= '1' && mode <= '9') || (mode >= 'A' && mode <= 'F')) return true;
    return cat->c4fm && (mode == 'H' || mode == 'I');
}

static char mode_or_default(char mode)
{
    return mode ? mode : '2';
}

size_t yaesu_cat_info(const cat_state_t *cat, bool sub, char out[YAESU_CAT_REPLY_MAX])
{
    const uint32_t hz = sub ? cat->freq_sub : cat->freq_main;
    const char mode = mode_or_default(sub ? cat->mode_sub_raw : cat->mode_main_raw);
    // FTDX10: 3-digit memory channel field. FTX-1: 5-digit field, 00000 for VFO.
    const char *channel = cat->rig == CAT_RIG_FTX1 ? "00000" : "001";
    const int n = snprintf(out, YAESU_CAT_REPLY_MAX, "%s%s%09lu+000000%c00000;",
                           sub ? "OI" : "IF", channel, (unsigned long)hz, mode);
    return n > 0 ? (size_t)n : 0;
}

static char tx_state(const cat_state_t *cat)
{
    if (cat->tx_cat) return '1';
    return cat->tx || cat->tx_radio ? '2' : '0';
}

static yaesu_reply_t read_command(const char *name, const char *p, const cat_state_t *cat,
                                  const yaesu_session_t *session, char *reply)
{
    char buf[YAESU_CAT_REPLY_MAX];
    const size_t pl = strlen(p);
    if (!strcmp(name, "ID") && !pl) {
        if (strlen(cat->rig_id) != 4) return rejected_read(reply);
        snprintf(buf, sizeof(buf), "ID%s;", cat->rig_id);
    } else if ((!strcmp(name, "FA") || !strcmp(name, "FB")) && !pl) {
        snprintf(buf, sizeof(buf), "%s%09lu;", name,
                 (unsigned long)(name[1] == 'A' ? cat->freq_main : cat->freq_sub));
    } else if ((!strcmp(name, "IF") || !strcmp(name, "OI")) && !pl) {
        yaesu_cat_info(cat, name[0] == 'O', buf);
    } else if (!strcmp(name, "MD") && pl == 1 && (p[0] == '0' || p[0] == '1')) {
        snprintf(buf, sizeof(buf), "MD%c%c;", p[0],
                 mode_or_default(p[0] == '1' ? cat->mode_sub_raw : cat->mode_main_raw));
    } else if (!strcmp(name, "FT") && !pl) {
        snprintf(buf, sizeof(buf), "FT%c;", cat->tx_vfo == CAT_TX_VFO_SUB ? '1' : '0');
    } else if (!strcmp(name, "ST") && !pl) {
        const bool rx_sub = cat->rx_vfo_known && cat->rx_vfo_sub;
        const bool split = cat->tx_vfo != CAT_TX_VFO_UNKNOWN && (cat->tx_vfo == CAT_TX_VFO_SUB) != rx_sub;
        snprintf(buf, sizeof(buf), "ST%c;", split ? '1' : '0');
    } else if (!strcmp(name, "VS") && !pl) {
        snprintf(buf, sizeof(buf), "VS%c;", cat->rx_vfo_known && cat->rx_vfo_sub ? '1' : '0');
    } else if (!strcmp(name, "TX") && !pl) {
        snprintf(buf, sizeof(buf), "TX%c;", tx_state(cat));
    } else if (!strcmp(name, "AI") && !pl) {
        snprintf(buf, sizeof(buf), "AI%c;", session->ai ? '1' : '0');
    } else if (!strcmp(name, "KP") && !pl) {
        // Until the radio's value is read, answer 600 Hz so fixed-length readers stay in step.
        snprintf(buf, sizeof(buf), "KP%02d;", cat->key_pitch >= 0 && cat->key_pitch <= 75 ? cat->key_pitch : 30);
    } else if (!strcmp(name, "PS") && !pl) {
        strcpy(buf, "PS1;");
    } else if (!strcmp(name, "SH") && pl == 1 && (p[0] == '0' || (p[0] == '1' && cat->dual_rx))) {
        const int code = cat->width_code[p[0] == '1'];
        if (code < 0) return rejected_read(reply);
        snprintf(buf, sizeof(buf), "SH%c0%02d;", p[0], code);
    } else if (!strcmp(name, "SM") && pl == 1 && (p[0] == '0' || (p[0] == '1' && cat->dual_rx))) {
        snprintf(buf, sizeof(buf), "SM%c%03u;", p[0], (unsigned)(p[0] == '1' ? cat->s_sub : cat->s_main));
    } else {
        return rejected_read(reply);
    }
    return text(reply, buf);
}

yaesu_reply_t yaesu_cat_command(const char *cmd, const cat_state_t *cat, yaesu_session_t *session,
                                yaesu_set_fn set, void *context, char reply[YAESU_CAT_REPLY_MAX])
{
    reply[0] = '\0';
    // A radio that is off or not yet identified does not answer at all.
    if (!cat->connected || strlen(cat->rig_id) != 4) return (yaesu_reply_t){.rejected = true};

    const size_t len = strlen(cmd);
    if (len < 2 || len > YAESU_CAT_CMD_MAX) return rejected_read(reply);
    char upper[YAESU_CAT_CMD_MAX + 1];
    for (size_t i = 0; i <= len; ++i) upper[i] = (char)toupper((unsigned char)cmd[i]);
    const char name[3] = {upper[0], upper[1], '\0'};
    const char *p = upper + 2;
    const size_t pl = len - 2;

    yaesu_set_t action = {0};
    if ((!strcmp(name, "FA") || !strcmp(name, "FB")) && pl == 9 && digits(p, 9)) {
        unsigned long hz = 0;
        sscanf(p, "%9lu", &hz);
        if (!cat->can_control || hz < cat->freq_min || hz > cat->freq_max) return write_result(reply, false);
        action = (yaesu_set_t){.action = YAESU_SET_FREQ, .sub = name[1] == 'B', .value = (uint32_t)hz};
    } else if (!strcmp(name, "MD") && pl == 2 && (p[0] == '0' || p[0] == '1')) {
        if (!cat->can_control || !valid_mode(cat, p[1])) return write_result(reply, false);
        action = (yaesu_set_t){.action = YAESU_SET_MODE, .sub = p[0] == '1', .value = (uint32_t)p[1]};
    } else if (!strcmp(name, "VS") && pl == 1) {
        if (!cat->can_control || (p[0] != '0' && p[0] != '1')) return write_result(reply, false);
        action = (yaesu_set_t){.action = YAESU_SELECT_VFO, .sub = p[0] == '1'};
    } else if (!strcmp(name, "FT") && pl == 1) {
        // The FTX-1 selects the transmitter with 0 / 1, the FTDX10 with 2 / 3.
        const char main = cat->dual_rx ? '0' : '2';
        if (!cat->can_control || (p[0] != main && p[0] != main + 1)) return write_result(reply, false);
        action = (yaesu_set_t){.action = YAESU_SET_TX_VFO, .sub = p[0] == main + 1};
    } else if (!strcmp(name, "ST") && pl == 1) {
        // Split on: transmit on the VFO that is not receiving. ST2 (split +5 kHz) is not offered.
        if (!cat->can_control || (p[0] != '0' && p[0] != '1')) return write_result(reply, false);
        const bool rx_sub = cat->rx_vfo_known && cat->rx_vfo_sub;
        action = (yaesu_set_t){.action = YAESU_SET_TX_VFO, .sub = p[0] == '1' ? !rx_sub : rx_sub};
    } else if ((!strcmp(name, "SV") || !strcmp(name, "AB") || !strcmp(name, "BA")) && !pl) {
        if (!cat->can_control) return write_result(reply, false);
        action.action = name[0] == 'S' ? YAESU_SWAP_VFO : name[0] == 'A' ? YAESU_COPY_A_TO_B : YAESU_COPY_B_TO_A;
    } else if (!strcmp(name, "AI") && pl == 1) {
        // Auto information is kept per client. The radio itself keeps AI1 for the board.
        if (p[0] != '0' && p[0] != '1') return write_result(reply, false);
        session->ai = p[0] == '1';
        return write_result(reply, true);
    } else if (!strcmp(name, "TX") && pl == 1) {
        // Receive is accepted without effect; the network client can never key the radio.
        return write_result(reply, p[0] == '0');
    } else if (pl && !(pl == 1 && (!strcmp(name, "MD") || !strcmp(name, "SM") || !strcmp(name, "SH")))) {
        // Any other write (power, menu, memory, keyer ...) is not forwarded.
        return write_result(reply, false);
    } else {
        return read_command(name, p, cat, session, reply);
    }
    return write_result(reply, set(&action, context));
}

size_t yaesu_cat_changes(const cat_state_t *old, const cat_state_t *now, char *out, size_t capacity)
{
    size_t used = 0;
    out[0] = '\0';
    if (!now->connected) return 0;
    if (old->freq_main != now->freq_main && now->freq_main && used < capacity) {
        used += (size_t)snprintf(out + used, capacity - used, "FA%09lu;", (unsigned long)now->freq_main);
    }
    if (old->freq_sub != now->freq_sub && now->freq_sub && used < capacity) {
        used += (size_t)snprintf(out + used, capacity - used, "FB%09lu;", (unsigned long)now->freq_sub);
    }
    if (old->mode_main_raw != now->mode_main_raw && now->mode_main_raw && used < capacity) {
        used += (size_t)snprintf(out + used, capacity - used, "MD0%c;", now->mode_main_raw);
    }
    if (old->mode_sub_raw != now->mode_sub_raw && now->mode_sub_raw && used < capacity) {
        used += (size_t)snprintf(out + used, capacity - used, "MD1%c;", now->mode_sub_raw);
    }
    return used < capacity ? used : capacity - 1;
}
