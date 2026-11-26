/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "rigctl_protocol.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    char code;
    const char *name;
    unsigned hamlib_bit;
} modes[] = {
    {'1', "LSB", 3}, {'2', "USB", 2}, {'3', "CW", 1}, {'4', "FM", 5},
    {'5', "AM", 0}, {'6', "RTTY", 4}, {'7', "CWR", 7}, {'8', "PKTLSB", 10},
    {'9', "RTTYR", 8}, {'A', "PKTFM", 12}, {'C', "PKTUSB", 11},
    {'B', "FMN", 21}, {'D', "AMN", 29}, {'F', "PKTFMN", 34},
};

const char *rigctl_mode_name(char code)
{
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        if (modes[i].code == code) return modes[i].name;
    }
    return NULL;
}

char rigctl_mode_code(const char *name)
{
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        if (strcmp(modes[i].name, name) == 0) return modes[i].code;
    }
    return 0;
}

static const uint16_t ssb_widths[] = {
    0, 300, 400, 600, 850, 1100, 1200, 1500, 1650, 1800, 1950, 2100,
    2250, 2400, 2450, 2500, 2600, 2700, 2800, 2900, 3000, 3200, 3500, 4000
};
static const uint16_t narrow_widths[] = {
    0, 50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 600,
    800, 1200, 1400, 1700, 2000, 2400, 3000, 3200, 3500, 4000
};

unsigned rigctl_width_hz(cat_rig_t rig, char mode, int code)
{
    if (!mode) return 0;
    // AM/FM widths are fixed by mode. The FTX-1 and FTDX10 use different AM widths.
    if (strchr("5D", mode)) return rig == CAT_RIG_FTDX10 && mode == '5' ? 9000 : 6000;
    if (strchr("BF", mode)) return 9000;
    if (strchr("4A", mode)) return 16000;
    if (code < 0) return 0;
    if (strchr("12", mode) && code < (int)(sizeof(ssb_widths) / sizeof(ssb_widths[0]))) {
        return ssb_widths[code];
    }
    if (strchr("36789CE", mode) &&
        code < (int)(sizeof(narrow_widths) / sizeof(narrow_widths[0]))) {
        return narrow_widths[code];
    }
    return 0;
}

int rigctl_width_code(cat_rig_t rig, char mode, int hz)
{
    if (hz <= 0 || !mode || !strchr("1236789CE", mode)) return -1;
    for (int code = 1; code <= 23; ++code) {
        if (rigctl_width_hz(rig, mode, code) == (unsigned)hz) return code;
    }
    return -1;
}

typedef enum {
    GET_FREQ, SET_FREQ, GET_MODE, SET_MODE, GET_VFO, SET_VFO, GET_SPLIT, SET_SPLIT,
    GET_PTT, GET_POWER, CHECK_VFO, DUMP_STATE, VFO_INFO, QUIT
} command_id_t;

static const struct {
    char short_name;
    const char *name;
    unsigned argc;
    const char *labels[2];
} commands[] = {
    [GET_FREQ] = {'f', "get_freq", 0, {"Frequency"}},
    [SET_FREQ] = {'F', "set_freq", 1},
    [GET_MODE] = {'m', "get_mode", 0, {"Mode", "Passband"}},
    [SET_MODE] = {'M', "set_mode", 2},
    [GET_VFO] = {'v', "get_vfo", 0, {"VFO"}},
    [SET_VFO] = {'V', "set_vfo", 1},
    [GET_SPLIT] = {'s', "get_split_vfo", 0, {"Split", "TX VFO"}},
    [SET_SPLIT] = {'S', "set_split_vfo", 2},
    [GET_PTT] = {'t', "get_ptt", 0, {"PTT"}},
    [GET_POWER] = {0, "get_powerstat", 0, {"Power Status"}},
    [CHECK_VFO] = {0, "chk_vfo", 0, {"ChkVFO"}},
    [DUMP_STATE] = {0, "dump_state", 0},
    [VFO_INFO] = {0, "get_vfo_info", 1},
    [QUIT] = {'q', "quit", 0},
};

static int find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (name[0] == '\\' ? strcmp(name + 1, commands[i].name) == 0
                            : name[0] == commands[i].short_name && name[1] == '\0') return (int)i;
    }
    return -1;
}

static bool parse_number(const char *str, long long min, long long max, long long *number)
{
    char *end;
    errno = 0;
    const long long value = strtoll(str, &end, 10);
    if (end == str || *end || errno || value < min || value > max) return false;
    *number = value;
    return true;
}

static bool parse_frequency(char *str, const cat_state_t *cat, long long *hz)
{
    // Hamlib's NET backend sends frequencies with a decimal suffix even for whole Hz.
    char *fraction = strchr(str, '.');
    if (fraction) {
        if (!fraction[1] || fraction[strspn(fraction + 1, "0") + 1]) return false;
        *fraction = '\0';
    }
    const bool valid = parse_number(str, cat->freq_min, cat->freq_max, hz);
    if (fraction) *fraction = '.';
    return valid;
}

static const char *vfo_name(const cat_state_t *cat, bool sub)
{
    return cat->rig == CAT_RIG_FTX1 ? (sub ? "Sub" : "Main") : (sub ? "VFOB" : "VFOA");
}

static int parse_vfo(const char *name)
{
    if (strcmp(name, "VFOA") == 0 || strcmp(name, "Main") == 0 || strcmp(name, "MAIN") == 0) return 0;
    if (strcmp(name, "VFOB") == 0 || strcmp(name, "Sub") == 0 || strcmp(name, "SUB") == 0) return 1;
    return -1;
}

static int dump_state(const cat_state_t *cat, char *out, size_t cap)
{
    uint64_t mode_mask = 0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        mode_mask |= UINT64_C(1) << modes[i].hamlib_bit;
    }
    // NET rigctl wire protocol v1. Terminate RX/TX ranges, steps and filters separately.
    return snprintf(out, cap, "1\n%u\n0\n"
                    "%" PRIu32 " %" PRIu32 " 0x%" PRIx64 " -1 -1 0x%x 0x0\n"
                    "0 0 0 0 0 0 0\n0 0 0 0 0 0 0\n0 0\n0 0\n"
                    "0\n0\n0\n0\n\n\n0x0\n0x0\n0x0\n0x0\n0x0\n0x0\n"
                    "vfo_ops=0x0\nptt_type=0x1\ntargetable_vfo=0x0\n"
                    "has_set_vfo=1\nhas_get_vfo=1\nhas_set_freq=1\nhas_get_freq=1\n"
                    "timeout=2000\nrig_model=%u\ndone\n",
                    cat->rig == CAT_RIG_FTDX10 ? 1042u : 1u,
                    cat->freq_min ? cat->freq_min : 30000u,
                    cat->freq_max ? cat->freq_max : 470000000u,
                    mode_mask, cat->rig == CAT_RIG_FTX1 ? 0x06000000u : 3u,
                    cat->rig == CAT_RIG_FTDX10 ? 1042u : 1u);
}

static rigctl_reply_t vfo_info(const cat_state_t *cat, const char *arg, bool extended, char *out)
{
    int target = parse_vfo(arg);
    if ((strcmp(arg, "currVFO") == 0 || strcmp(arg, "VFO") == 0) && cat->rx_vfo_known) {
        target = cat->rx_vfo_sub;
    }
    if (strcmp(arg, "?") == 0) {
        const int len = snprintf(out, RIGCTL_REPLY_SIZE, "%s%s %s\n%s",
                                 extended ? "get_vfo_info: ?\n" : "", vfo_name(cat, false),
                                 vfo_name(cat, true), extended ? "RPRT 0\n" : "");
        return (rigctl_reply_t){.length = (size_t)len};
    }
    const uint32_t hz = target == 1 ? cat->freq_sub : cat->freq_main;
    const char mode = target == 1 ? cat->mode_sub_raw : cat->mode_main_raw;
    const char *name = rigctl_mode_name(mode);
    if (!cat->connected || target < 0 || !hz || !name || !cat->rx_vfo_known ||
        cat->tx_vfo == CAT_TX_VFO_UNKNOWN) {
        strcpy(out, "RPRT -11\n");
        return (rigctl_reply_t){.length = sizeof("RPRT -11\n") - 1, .rejected = true};
    }
    const unsigned width = rigctl_width_hz(cat->rig, mode, cat->width_code[target]);
    const unsigned split = (cat->tx_vfo == CAT_TX_VFO_SUB) != cat->rx_vfo_sub;
    const int len = extended
        ? snprintf(out, RIGCTL_REPLY_SIZE,
                   "get_vfo_info: %s\nFreq: %" PRIu32 "\nMode: %s\nWidth: %u\nSplit: %u\nSatMode: 0\nRPRT 0\n",
                   arg, hz, name, width, split)
        : snprintf(out, RIGCTL_REPLY_SIZE, "%" PRIu32 "\n%s\n%u\n%u\n0\n", hz, name, width, split);
    return (rigctl_reply_t){.length = (size_t)len};
}

static int execute(int cmd, char *const args[], const cat_state_t *cat, rigctl_set_fn set, void *context,
                   char first[24], char second[24])
{
    if (cmd == CHECK_VFO || cmd == GET_POWER) {
        strcpy(first, cmd == GET_POWER && cat->connected ? "1" : "0");
        return 0;
    }
    if (!cat->connected) return -11;
    if (cmd == GET_PTT) {
        if (!cat->tx_radio_known) return -11;
        strcpy(first, cat->tx_radio ? "1" : "0");
        return 0;
    }
    if (!cat->rx_vfo_known && cmd != SET_VFO) return -11;
    const bool sub = cat->rx_vfo_sub;
    const char mode = sub ? cat->mode_sub_raw : cat->mode_main_raw;
    const char *name;
    long long number, width;
    int target;
    rigctl_set_t update = {.sub = sub};

    switch (cmd) {
    case GET_FREQ: {
        const uint32_t hz = sub ? cat->freq_sub : cat->freq_main;
        if (!hz) return -11;
        snprintf(first, 24, "%" PRIu32, hz);
        return 0;
    }
    case GET_MODE:
        name = rigctl_mode_name(mode);
        if (!name) return -11;
        strcpy(first, name);
        snprintf(second, 24, "%u", rigctl_width_hz(cat->rig, mode, cat->width_code[sub]));
        return 0;
    case GET_VFO:
        strcpy(first, vfo_name(cat, sub));
        return 0;
    case GET_SPLIT:
        if (cat->tx_vfo == CAT_TX_VFO_UNKNOWN) return -11;
        target = cat->tx_vfo == CAT_TX_VFO_SUB;
        strcpy(first, target != sub ? "1" : "0");
        strcpy(second, vfo_name(cat, target));
        return 0;
    case SET_FREQ:
        if (!parse_frequency(args[0], cat, &number)) return -1;
        update.action = RIGCTL_FREQ;
        update.value = (uint32_t)number;
        break;
    case SET_MODE:
        update.action = RIGCTL_MODE;
        update.value = rigctl_mode_code(args[0]);
        if (!update.value || !parse_number(args[1], -1, UINT16_MAX, &width)) return -1;
        update.width = (int)width;
        break;
    case SET_VFO:
        target = parse_vfo(args[0]);
        if (target < 0) return -1;
        update.action = RIGCTL_VFO;
        update.sub = target;
        break;
    case SET_SPLIT:
        target = parse_vfo(args[1]);
        if (!parse_number(args[0], 0, 1, &number) ||
            (target < 0 && strcmp(args[1], "None") != 0) ||
            (number && (target < 0 || target == sub))) return -1;
        update.action = RIGCTL_SPLIT;
        update.sub = number ? target : sub;
        break;
    default:
        return -11;
    }

    if (!cat->can_control || !cat->tx_radio_known || cat->tx_radio || cat->tx || cat->tx_cat) return -11;
    return set(&update, context) ? 0 : -11;
}

rigctl_reply_t rigctl_command(char *line, const cat_state_t *cat, rigctl_set_fn set, void *context,
                             char reply[RIGCTL_REPLY_SIZE])
{
    char *save;
    char *token = strtok_r(line, " \t\r", &save);
    if (!token) return (rigctl_reply_t){0};
    const bool extended = token[0] == '+';
    if (extended && !*++token) token = strtok_r(NULL, " \t\r", &save);
    const int cmd = token ? find_command(token) : -1;
    char *args[2] = {0};
    unsigned argc = 0;
    while (argc < 2 && (args[argc] = strtok_r(NULL, " \t\r", &save))) ++argc;
    const bool extra = argc == 2 && strtok_r(NULL, " \t\r", &save) != NULL;
    char first[24] = "", second[24] = "";
    int err = -11;

    if (cmd >= 0) {
        if (extra || argc != commands[cmd].argc) {
            err = -1;
        } else if (cmd == QUIT) {
            strcpy(reply, "RPRT 0\n");
            return (rigctl_reply_t){.length = sizeof("RPRT 0\n") - 1, .close = true};
        } else if (cmd == VFO_INFO) {
            return vfo_info(cat, args[0], extended, reply);
        } else if (cmd == DUMP_STATE) {
            size_t used = 0;
            if (extended) used = (size_t)snprintf(reply, RIGCTL_REPLY_SIZE, "dump_state:\n");
            used += dump_state(cat, reply + used, RIGCTL_REPLY_SIZE - used);
            if (extended) used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "RPRT 0\n");
            return (rigctl_reply_t){.length = used};
        } else {
            err = execute(cmd, args, cat, set, context, first, second);
        }
    }

    size_t used = 0;
    if (extended && cmd >= 0 && cmd != CHECK_VFO) {
        used += (size_t)snprintf(reply, RIGCTL_REPLY_SIZE, "%s:", commands[cmd].name);
        for (unsigned i = 0; i < argc; ++i) {
            used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, " %s", args[i]);
        }
        reply[used++] = '\n';
    }
    const char *values[] = {first, second};
    for (unsigned i = 0; err == 0 && i < 2 && *values[i]; ++i) {
        if (extended) {
            used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "%s: ", commands[cmd].labels[i]);
        }
        used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "%s\n", values[i]);
    }
    if (extended || err || !*first) {
        used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "RPRT %d\n", err);
    }
    return (rigctl_reply_t){.length = used, .rejected = err != 0};
}
