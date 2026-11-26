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
    {'5', "AM", 0}, {'3', "CW", 1}, {'2', "USB", 2}, {'1', "LSB", 3},
    {'6', "RTTY", 4}, {'4', "FM", 5}, {'7', "CWR", 7}, {'9', "RTTYR", 8},
    {'8', "PKTLSB", 10}, {'C', "PKTUSB", 11}, {'A', "PKTFM", 12},
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
    GET_PTT, GET_POWER, CHECK_VFO, DUMP_STATE, VFO_INFO, RIG_INFO, VFO_LIST, GET_LOCK, GET_LEVEL, SET_LEVEL, SET_FUNC, GET_FUNC, VFO_OP, SET_PARM, GET_PARM, QUIT
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
    [RIG_INFO] = {0, "get_rig_info", 0},
    [VFO_LIST] = {0, "get_vfo_list", 0, {"VFO list"}},
    [GET_LOCK] = {0, "get_lock_mode", 0, {"Locked"}},
    [GET_LEVEL] = {'l', "get_level", 1},
    [SET_LEVEL] = {'L', "set_level", 2},
    [SET_FUNC] = {'U', "set_func", 2},
    [GET_FUNC] = {'u', "get_func", 1},
    [VFO_OP] = {'G', "vfo_op", 1},
    [SET_PARM] = {'P', "set_parm", 2},
    [GET_PARM] = {'p', "get_parm", 1},
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

static unsigned hamlib_model(const cat_state_t *cat)
{
    // Hamlib 4.6 has no native FTX-1 model. Use its generic Dummy identifier.
    return cat->rig == CAT_RIG_FTDX10 ? 1042u : 1u;
}

static int read_vfo(const cat_state_t *cat, const char *name)
{
    if (!name || strcmp(name, "currVFO") == 0 || strcmp(name, "VFO") == 0) {
        return cat->rx_vfo_known ? cat->rx_vfo_sub : -1;
    }
    return parse_vfo(name);
}

static bool supports_vfo_argument(int cmd)
{
    return cmd == GET_FREQ || cmd == GET_MODE || cmd == GET_VFO || cmd == GET_SPLIT ||
           cmd == GET_PTT || cmd == GET_LEVEL ||
           cmd == SET_FREQ || cmd == SET_MODE || cmd == SET_SPLIT;
}

// Hamlib ends the extended reply with RPRT on either kind of separator.
static rigctl_reply_t capability_reply(int cmd, char sep, const cat_state_t *cat, char *out)
{
    size_t used = 0;
    const char *list = "";
    char vfos[32];
    if (cmd == SET_VFO || cmd == VFO_INFO) {
        snprintf(vfos, sizeof(vfos), "%s %s currVFO ", vfo_name(cat, false), vfo_name(cat, true));
        list = vfos;
    } else if (cmd == GET_LEVEL) {
        list = "RAWSTR STRENGTH ";
    }
    if (sep) used = (size_t)snprintf(out, RIGCTL_REPLY_SIZE, "%s: ?%c", commands[cmd].name, sep);
    if (cmd == SET_MODE) {
        // Hamlib prints mode names in capability bit order, without a trailing space.
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            used += (size_t)snprintf(out + used, RIGCTL_REPLY_SIZE - used, "%s%s", i ? " " : "", modes[i].name);
        }
    }
    // Capability lists contain a literal newline. get_level has an extra
    // separator after its list in Hamlib 4.6, even in the default mode.
    const char level_sep[] = {cmd == GET_LEVEL ? (sep ? sep : '\n') : '\0', '\0'};
    const bool status = sep || cmd == SET_VFO || cmd == SET_MODE || cmd == SET_LEVEL ||
                        cmd == SET_FUNC || cmd == VFO_OP || cmd == SET_PARM;
    used += (size_t)snprintf(out + used, RIGCTL_REPLY_SIZE - used, "%s%s\n%s",
                             list, level_sep, status ? "RPRT 0\n" : "");
    return (rigctl_reply_t){.length = used};
}

static bool supports_capability_query(int cmd)
{
    return cmd == SET_VFO || cmd == VFO_INFO || cmd == SET_MODE || cmd == GET_LEVEL ||
           cmd == SET_LEVEL || cmd == SET_FUNC || cmd == GET_FUNC || cmd == VFO_OP ||
           cmd == SET_PARM || cmd == GET_PARM;
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
                    "0\n0\n0\n0\n\n\n0x0\n0x0\n0x44000000\n0x0\n0x0\n0x0\n"
                    "vfo_ops=0x0\nptt_type=0x1\ntargetable_vfo=0x0\n"
                    "has_set_vfo=1\nhas_get_vfo=1\nhas_set_freq=1\nhas_get_freq=1\n"
                    "timeout=2000\nrig_model=%u\ndone\n",
                    hamlib_model(cat),
                    cat->freq_min ? cat->freq_min : 30000u,
                    cat->freq_max ? cat->freq_max : 470000000u,
                    mode_mask, cat->rig == CAT_RIG_FTX1 ? 0x06000000u : 3u,
                    hamlib_model(cat));
}

static uint32_t rig_info_crc(const char *text, size_t length)
{
    // CRC-32/ISO-HDLC over the body including the newline immediately before CRC=.
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint8_t)text[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return crc ^ UINT32_MAX;
}

static rigctl_reply_t rig_info(const cat_state_t *cat, char sep, char *out)
{
    // Hamlib keeps the body and CRC byte-for-byte in all response modes.
    const size_t header_len = sep ? (size_t)snprintf(out, RIGCTL_REPLY_SIZE, "get_rig_info:%c", sep) : 0;
    size_t used = header_len;
    const bool rx_known = cat->connected && cat->rx_vfo_known;
    const bool tx_known = cat->connected && cat->tx_vfo != CAT_TX_VFO_UNKNOWN;
    const bool tx_sub = cat->tx_vfo == CAT_TX_VFO_SUB;
    for (unsigned sub = 0; sub < 2; ++sub) {
        const char mode = cat->connected ? (sub ? cat->mode_sub_raw : cat->mode_main_raw) : 0;
        const char *name = rigctl_mode_name(mode);
        const uint32_t freq = cat->connected ? (sub ? cat->freq_sub : cat->freq_main) : 0;
        used += (size_t)snprintf(out + used, RIGCTL_REPLY_SIZE - used,
                                 "VFO=%s Freq=%" PRIu32 " Mode=%s Width=%u RX=%u TX=%u\n",
                                 vfo_name(cat, sub), freq, name ? name : "None",
                                 rigctl_width_hz(cat->rig, mode, cat->width_code[sub]),
                                 (unsigned)(rx_known && sub == cat->rx_vfo_sub),
                                 (unsigned)(tx_known && sub == tx_sub));
    }
    const char *rig = cat->rig == CAT_RIG_FTX1 ? "FTX-1" :
                      cat->rig == CAT_RIG_FTDX10 ? "FTDX10" : "Network Remote";
    used += (size_t)snprintf(out + used, RIGCTL_REPLY_SIZE - used,
                             "Split=%u SatMode=0\nRig=%s\nApp=Network Remote\n"
                             "Version=20241103 1.1.0\nModel=%u\n",
                             (unsigned)(rx_known && tx_known && cat->rx_vfo_sub != tx_sub),
                             rig, hamlib_model(cat));
    const uint32_t crc = rig_info_crc(out + header_len, used - header_len);
    used += (size_t)snprintf(out + used, RIGCTL_REPLY_SIZE - used,
                             "CRC=0x%08" PRIx32 "\n\n%s", crc, sep ? "RPRT 0\n" : "");
    return (rigctl_reply_t){.length = used};
}

static rigctl_reply_t vfo_info(const cat_state_t *cat, const char *arg, char sep, char *out)
{
    const int target = read_vfo(cat, arg);
    const uint32_t hz = target == 1 ? cat->freq_sub : cat->freq_main;
    const char mode = target == 1 ? cat->mode_sub_raw : cat->mode_main_raw;
    const char *name = rigctl_mode_name(mode);
    if (!cat->connected || target < 0 || !hz || !name || !cat->rx_vfo_known ||
        cat->tx_vfo == CAT_TX_VFO_UNKNOWN) {
        const int len = sep ? snprintf(out, RIGCTL_REPLY_SIZE, "get_vfo_info: %s%cRPRT -11\n", arg, sep)
                            : snprintf(out, RIGCTL_REPLY_SIZE, "RPRT -11\n");
        return (rigctl_reply_t){.length = (size_t)len, .rejected = true};
    }
    const unsigned width = rigctl_width_hz(cat->rig, mode, cat->width_code[target]);
    const unsigned split = (cat->tx_vfo == CAT_TX_VFO_SUB) != cat->rx_vfo_sub;
    const int len = sep
        ? snprintf(out, RIGCTL_REPLY_SIZE,
                   "get_vfo_info: %s%cFreq: %" PRIu32 "%cMode: %s%cWidth: %u%cSplit: %u%cSatMode: 0%cRPRT 0\n",
                   arg, sep, hz, sep, name, sep, width, sep, split, sep, sep)
        : snprintf(out, RIGCTL_REPLY_SIZE, "%" PRIu32 "\n%s\n%u\n%u\n0\n", hz, name, width, split);
    return (rigctl_reply_t){.length = (size_t)len};
}

static int execute(int cmd, char *const args[], const char *vfo, const cat_state_t *cat,
                   rigctl_set_fn set, void *context,
                   char first[24], char second[24])
{
    if (cmd == GET_POWER || cmd == GET_LOCK) {
        strcpy(first, cmd == GET_POWER && cat->connected ? "1" : "0");
        return 0;
    }
    if (cmd == VFO_LIST) {
        snprintf(first, 24, "%s %s", vfo_name(cat, false), vfo_name(cat, true));
        return 0;
    }
    if (!cat->connected) return -11;
    const int side = read_vfo(cat, vfo);
    if (vfo && side < 0) return -1;
    if (cmd == GET_PTT) {
        if (!cat->tx_radio_known) return -11;
        strcpy(first, cat->tx_radio ? "1" : "0");
        return 0;
    }
    if (side < 0 && cmd != SET_VFO) return -11;
    const bool sub = side == 1;
    const char mode = sub ? cat->mode_sub_raw : cat->mode_main_raw;
    const char *name;
    long long number, width;
    int target;
    rigctl_set_t update = {.sub = sub, .require_active = !vfo || parse_vfo(vfo) < 0};

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
        if (!cat->rx_vfo_known) return -11;
        strcpy(first, vfo_name(cat, cat->rx_vfo_sub));
        return 0;
    case GET_SPLIT:
        if (cat->tx_vfo == CAT_TX_VFO_UNKNOWN || !cat->rx_vfo_known) return -11;
        target = cat->tx_vfo == CAT_TX_VFO_SUB;
        strcpy(first, target != cat->rx_vfo_sub ? "1" : "0");
        strcpy(second, vfo_name(cat, target));
        return 0;
    case GET_LEVEL: {
        if (!cat->dual_rx && (!cat->rx_vfo_known || sub != cat->rx_vfo_sub)) return -11;
        const int raw = sub && cat->dual_rx ? cat->s_sub : cat->s_main;
        if (strcmp(args[0], "RAWSTR") == 0) {
            snprintf(first, 24, "%d", raw);
        } else if (strcmp(args[0], "STRENGTH") == 0) {
            // Match the web S-meter scale (S9 at 120 and S9+60 at 255).
            const int db = raw <= 120 ? (raw - 120) * 54 / 120 : (raw - 120) * 60 / 135;
            snprintf(first, 24, "%d", db);
        } else {
            return -11;
        }
        return 0;
    }
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
        target = read_vfo(cat, args[1]);
        if (!parse_number(args[0], 0, 1, &number) ||
            (target < 0 && strcmp(args[1], "None") != 0) ||
            (number && (target < 0 || target == sub))) return -1;
        update.action = RIGCTL_SPLIT;
        update.value = number ? (unsigned)target : (unsigned)sub;
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
    const char prefix = token[0];
    const char sep = prefix == '+' ? '\n' : strchr(";,|", prefix) ? prefix : 0;
    if (sep && !*++token) token = strtok_r(NULL, " \t\r", &save);
    const int cmd = token ? find_command(token) : -1;
    char *args[3] = {0};
    unsigned argc = 0;
    const unsigned max_args = sizeof(args) / sizeof(args[0]);
    while (argc < max_args && (args[argc] = strtok_r(NULL, " \t\r", &save))) ++argc;
    const bool extra = argc == max_args && strtok_r(NULL, " \t\r", &save) != NULL;
    char first[24] = "", second[24] = "";
    int err = -11;

    if (cmd >= 0) {
        const bool with_vfo = supports_vfo_argument(cmd) && argc == commands[cmd].argc + 1;
        if (supports_capability_query(cmd) && argc == 1 && !extra && strcmp(args[0], "?") == 0) {
            return capability_reply(cmd, sep, cat, reply);
        }
        if (extra || (argc != commands[cmd].argc && !with_vfo)) {
            err = -1;
        } else if (cmd == QUIT) {
            strcpy(reply, "RPRT 0\n");
            return (rigctl_reply_t){.length = sizeof("RPRT 0\n") - 1, .close = true};
        } else if (cmd == RIG_INFO) {
            return rig_info(cat, sep, reply);
        } else if (cmd == VFO_INFO) {
            return vfo_info(cat, args[0], sep, reply);
        } else if (cmd == DUMP_STATE) {
            size_t used = 0;
            if (sep) used = (size_t)snprintf(reply, RIGCTL_REPLY_SIZE, "dump_state:%c", sep);
            used += dump_state(cat, reply + used, RIGCTL_REPLY_SIZE - used);
            if (sep) used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "RPRT 0\n");
            return (rigctl_reply_t){.length = used};
        } else if (cmd == CHECK_VFO) {
            const int len = snprintf(reply, RIGCTL_REPLY_SIZE, "%s0\n%s",
                                     sep ? "ChkVFO: " : "", sep && sep != '\n' ? "\n" : "");
            return (rigctl_reply_t){.length = (size_t)len};
        } else {
            err = execute(cmd, args + with_vfo, with_vfo ? args[0] : NULL, cat, set, context, first, second);
        }
    }

    size_t used = 0;
    if (sep && cmd >= 0) {
        used += (size_t)snprintf(reply, RIGCTL_REPLY_SIZE, "%s:", commands[cmd].name);
        for (unsigned i = 0; i < argc; ++i) {
            used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, " %s", args[i]);
        }
        reply[used++] = sep;
    }
    const char *values[] = {first, second};
    for (unsigned i = 0; err == 0 && i < 2 && *values[i]; ++i) {
        if (sep && commands[cmd].labels[i]) {
            used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "%s: ", commands[cmd].labels[i]);
        }
        used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "%s%c", values[i], sep ? sep : '\n');
    }
    if (sep || err || !*first) {
        used += (size_t)snprintf(reply + used, RIGCTL_REPLY_SIZE - used, "RPRT %d\n", err);
    }
    return (rigctl_reply_t){.length = used, .rejected = err != 0};
}
