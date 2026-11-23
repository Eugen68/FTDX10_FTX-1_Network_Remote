/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * CAT control for the Yaesu FTX-1 and the Yaesu FTDX10.
 * Finds the radio's CAT serial port over USB, recognises the radio from its answer to "ID;", keeps a copy of
 * the radio's state (frequency, mode, S meter, ...) up to date, and sends the control commands requested
 * from the browser.
 *
 * Everything that depends on the radio model is in one rig_profile_t (see s_profiles[] below): the ID,
 * the frequency range, whether there is a SUB receiver, the squelch range, the menu (EX) item that is
 * written, and the commands that are polled. A radio that answers "ID;" with an unknown model gets a
 * read-only profile (frequency and mode are read, nothing is ever set).
 *
 * CAT commands used by this file, for comparing with a radio's CAT manual.
 * Every command is "<2 letters><parameters>;".  R = receiver: 0 = MAIN, 1 = SUB.  n = a digit.
 * Everything that is sent passes cat_cmd_allowed(); a command that is not accepted there is never sent.
 * Unless marked otherwise, a line applies to both radios.
 *
 *   Command         Dir    Meaning (as used here)
 *   --------------  -----  ------------------------------------------------------------------
 *   ID;             read   identify; the port that answers is used as the CAT port
 *   AI1;            set    auto information ON (the radio pushes changes by itself)
 *   FA; FB;         read   frequency of MAIN / VFO-A (FA) and SUB / VFO-B (FB). Answer: FA/FB + digits, Hz
 *   FAnnnnnnnnn;    set    frequency in Hz, 9 digits (FB for SUB). FTX-1 30000 - 470000000,
 *                          FTDX10 30000 - 75000000
 *   MDR;            read   mode of R (FTDX10: 1 = VFO-B). Answer: MDR + one mode code (see s_modes[])
 *   MDRc;           set    mode; c is one of the codes in s_modes[] (H and I, C4FM, on the FTX-1 only)
 *   AGR;            read   AF gain of receiver R. Answer: AGR + 3 digits (000 - 255). FTDX10: R = 0 only
 *   AGRnnn;         set    AF gain, 000 - 255
 *   SQR;            read   squelch level of receiver R. Answer: SQR + 3 digits. FTDX10: R = 0 only
 *   SQRnnn;         set    squelch level, FTX-1 000 - 255, FTDX10 000 - 100
 *   BPR0;  BPR1;    read   manual notch of receiver R: kind 0 = ON/OFF, kind 1 = frequency. FTDX10: R = 0 only
 *                          Answer: BPR0nnn (000 = off, 001 = on) / BPR1nnn (001 - 320, 10 Hz steps)
 *   BPR0nnn; BPR1nnn;  set the same two values
 *   SV;             set    swap MAIN and SUB (FTDX10: VFO-A and VFO-B)
 *   AB;             set    copy MAIN to SUB
 *   BA;             set    copy SUB to MAIN
 *   PSn;            set    power, 0 = OFF, 1 = ON (the browser's POWER button)
 *
 *   FTX-1 only (ID 0840):
 *   RI0;            read   radio information. Answer: "RI" + parameters P1 - P8 + ";" (11 characters);
 *                          P4 = transmitting (1 = yes), P8 (the last) = squelch open (1 = BUSY)
 *   RM0;            read   S meter. Answer: RM0 + 3 digits MAIN + 3 digits SUB (000 - 255)
 *   EX010411nnn;    set    menu RADIO SETTING / MODE DATA / USB OUT LEVEL, 000 - 100
 *
 *   FTDX10 only (ID 0761):
 *   BY;             read   busy. Answer: BY + P1 (1 = squelch open) + P2 (fixed 0) + ";"
 *   TX;             read   TX state. Answer: TX + P1 + ";" (0 = receive, 2 = the radio transmits).
 *                          Only the read form (no parameter) is allowed; "TX1;" is never sent.
 *   RM1;            read   S meter. Answer: RM1 + 3 digits (000 - 255) + "000;"
 *   EX010413nnn;    set    menu RADIO SETTING / MODE PSK/DATA / DATA OUT LEVEL, 000 - 100
 *                          (on the FTDX10, 010411 is HCUT FREQ: the menu numbers differ between the radios)
 *
 *   The EX item is set to 100 whenever a receiver is in DATA-USB; it is never read.
 *
 * Radio-specific values in this file: the profiles in s_profiles[], the mode table s_modes[], the answer
 * parsers (parse_freq, parse_mode, parse_gain, parse_notch, parse_ri, parse_rm, parse_by, parse_tx) and how
 * the CAT port is found (Silicon Labs CP210x, VID 0x10C4, the one that answers "ID;").
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "cat.h"

// The FTX-1 exposes two CP210x UARTs (Silicon Labs VID 0x10C4). Both have the same VID/PID, so they are
// opened by USB device address. Whichever answers "ID;" at CAT_BAUD is used as the CAT port.
#define SILABS_VID          0x10C4
#define CAT_BAUD            115200   // CAT-1 RATE of this radio
#define CAT_TASK_PRIORITY   4
#define POLL_PERIOD_MS      100
#define ANSWER_TIMEOUT_MS   300
#define TX_TIMEOUT_MS       200
#define RX_BUF_BYTES        64
#define MAX_FAILS           10       // consecutive missing answers before the port is dropped
#define SCAN_PERIOD_MS      2000

// CP210x vendor requests (Silicon Labs AN571)
#define CP210X_REQ_OUT      (USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_INTERFACE | USB_BM_REQUEST_TYPE_DIR_OUT)
#define CP210X_IFC_ENABLE   0x00
#define CP210X_SET_LINE_CTL 0x03
#define CP210X_SET_MHS      0x07
#define CP210X_SET_BAUDRATE 0x1E
#define CP210X_LINE_8N1     0x0800   // 8 data bits, no parity, 1 stop bit
#define CP210X_MHS_DTR_RTS_LOW 0x0300 // write mask for DTR and RTS, both cleared

static const char *TAG = "cat";

static SemaphoreHandle_t s_lock;       // protects s_state
static cat_state_t s_state;
static int64_t s_last_answer_us;
static int64_t s_last_set_main_sq_us;
static int64_t s_last_set_sub_sq_us;
static int64_t s_last_set_notch_us[2];       // [MAIN, SUB]: last local BP write (ON/OFF or frequency)
static int64_t s_last_set_main_vol_us;
static int64_t s_last_set_sub_vol_us;
static QueueHandle_t s_cmd_queue;      // queued commands to send over CAT

typedef struct {
    char cmd[32];
} cat_cmd_item_t;

static const struct {
    char code;
    const char *name;
} s_modes[] = {
    {'1', "LSB"},
    {'2', "USB"},
    {'3', "CW-U"},
    {'4', "FM"},
    {'5', "AM"},
    {'6', "RTTY-LSB"},
    {'7', "CW-L"},
    {'8', "DATA-LSB"},
    {'9', "RTTY-USB"},
    {'A', "DATA-FM"},
    {'B', "FM-N"},
    {'C', "DATA-USB"},
    {'D', "AM-N"},
    {'E', "PSK"},
    {'F', "DATA-FM-N"},
    {'H', "C4FM-DN"},   // FTX-1 only
    {'I', "C4FM-VW"},   // FTX-1 only
};

// ---- Radio profiles ---------------------------------------------------------------------------------
// The polled command strings must pass cat_cmds_allowed() for the same profile (at most CAT_TX_MAX_CMDS
// commands and CAT_TX_MAX_BYTES bytes).
typedef struct {
    cat_rig_t rig;
    const char *name;
    const char *id;           // the 4 characters after "ID" in the answer; NULL for the generic profiles
    uint32_t freq_min;        // FA / FB range, Hz
    uint32_t freq_max;
    bool dual_rx;             // AG / SQ / BP / S meter exist for a SUB receiver (R = 1)
    bool can_control;         // set commands are allowed at all
    bool c4fm;                // mode codes H and I exist
    bool has_ri;              // RI0; (FTX-1 radio information: BUSY and TX)
    bool has_by_tx;           // BY; and the read form of TX; (FTDX10)
    char meter_side;          // the P1 of "RM" that reads the S meter, 0 if none
    unsigned sq_max;          // highest SQ value
    const char *ex_data_out;  // menu item P1 P2 P3 set to DATA_USB_OUT_LEVEL in DATA-USB; NULL = none
    const char *poll_fast;    // every POLL_PERIOD_MS (S meter); NULL = none
    const char *poll_status;  // every 5 polls (BUSY / TX); NULL = none
    const char *sync_all;     // when the port is opened, then every 50 polls
    const char *resync_vfo;   // after SV / AB / BA
} rig_profile_t;

// No port open yet: only the identification query may be sent
static const rig_profile_t s_profile_none = {
    .rig = CAT_RIG_NONE, .name = "",
};

static const rig_profile_t s_profiles[] = {
    {
        .rig = CAT_RIG_FTX1, .name = "FTX-1", .id = "0840",
        .freq_min = 30000U, .freq_max = 470000000U,
        .dual_rx = true, .can_control = true, .c4fm = true, .has_ri = true, .has_by_tx = false,
        .meter_side = '0', .sq_max = 255,
        .ex_data_out = "010411",      // RADIO SETTING / MODE DATA / USB OUT LEVEL
        .poll_fast = "RM0;",
        .poll_status = "RI0;",
        .sync_all = "AI1;FA;FB;MD0;MD1;AG0;AG1;SQ0;SQ1;BP00;BP01;BP10;BP11;",
        .resync_vfo = "FA;FB;MD0;MD1;",
    },
    {
        .rig = CAT_RIG_FTDX10, .name = "FTDX10", .id = "0761",
        .freq_min = 30000U, .freq_max = 75000000U,
        .dual_rx = false, .can_control = true, .c4fm = false, .has_ri = false, .has_by_tx = true,
        .meter_side = '1', .sq_max = 100,
        .ex_data_out = "010413",      // RADIO SETTING / MODE PSK/DATA / DATA OUT LEVEL
        .poll_fast = "RM1;",
        .poll_status = "BY;TX;",
        .sync_all = "AI1;FA;FB;MD0;MD1;AG0;SQ0;BP00;BP01;",
        .resync_vfo = "FA;FB;MD0;MD1;",
    },
};

// A radio that answered "ID;" with a model not listed above: frequency and mode are read, nothing is set
static const rig_profile_t s_profile_unknown = {
    .rig = CAT_RIG_UNKNOWN, .name = "UNKNOWN",
    .freq_min = 30000U, .freq_max = 470000000U,
    .poll_status = "FA;FB;MD0;",
    .sync_all = "FA;FB;MD0;MD1;",
    .resync_vfo = "FA;FB;MD0;MD1;",
};

// Written only by cat_task (before the port is polled), read by every task that builds or sends a command
static const rig_profile_t *volatile s_rig = &s_profile_none;
static char s_rig_id[5];

static const rig_profile_t *profile_for_id(const char *id)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if (strncmp(s_profiles[i].id, id, 4) == 0) {
            return &s_profiles[i];
        }
    }
    return &s_profile_unknown;
}

static void diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void diag_counts(uint8_t devices, uint8_t cp210x, uint8_t answered);

static TaskHandle_t s_task;
static char s_rx[RX_BUF_BYTES];        // RX assembler, written only from the CDC driver task
static size_t s_rx_len;
static char s_answer[RX_BUF_BYTES];
static volatile bool s_got_answer;
static volatile uint32_t s_rx_bytes;     // all bytes received from the open port (diagnostics)
static char s_rx_tail[17];               // last received bytes, printable only
static volatile bool s_disconnected;

static void diag(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vsnprintf(s_state.last_error, sizeof(s_state.last_error), fmt, ap);
    xSemaphoreGive(s_lock);
    va_end(ap);
}

static void diag_counts(uint8_t devices, uint8_t cp210x, uint8_t answered)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.devices = devices;
    s_state.cp210x = cp210x;
    s_state.answered = answered;
    xSemaphoreGive(s_lock);
}

static void handle_cat_packet(const char *pkt);
static void log_add(char dir, uint8_t kind, const char *text, size_t len);
static bool is_poll_answer(const char *pkt);

static bool rx_cb(const uint8_t *data, size_t len, void *arg)
{
    s_rx_bytes += len;
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        memmove(s_rx_tail, s_rx_tail + 1, sizeof(s_rx_tail) - 2);
        s_rx_tail[sizeof(s_rx_tail) - 2] = (c >= 0x20 && c < 0x7f) ? c : '.';
        if (s_rx_len >= sizeof(s_rx) - 1) {
            s_rx_len = 0;              // overlong garbage: resync
        }
        s_rx[s_rx_len++] = c;
        if (c == ';') {
            s_rx[s_rx_len] = '\0';
            log_add('R', is_poll_answer(s_rx) ? CAT_LOG_POLL : CAT_LOG_ANSWER, s_rx, s_rx_len);
            memcpy(s_answer, s_rx, s_rx_len + 1);
            handle_cat_packet(s_rx);
            s_rx_len = 0;
            s_got_answer = true;
            if (s_task) {
                xTaskNotifyGive(s_task);
            }
        }
    }
    return true;
}

static void event_cb(const cdc_acm_host_dev_event_data_t *event, void *arg)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        s_disconnected = true;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
}

// ---- Transmit allow-list ----------------------------------------------------------------------------
// Every byte sent to the radio goes through cat_tx(), and cat_tx() only lets the commands below pass,
// checked against the profile of the radio that is connected. Anything else (TX with a parameter, MX, KY,
// PB, VX, PC, FT, GP, ...) is dropped, so this firmware has no way to make the radio transmit. EX (menu) is
// allowed for exactly one item per radio (the audio level sent to the PC in DATA mode), and nothing else
// in the radio's menu.
#define CAT_TX_MAX_BYTES    64      // matches out_buffer_size of the CDC port
#define CAT_CMD_MAX_BYTES   12      // longest allowed command: "FA" + 9 digits + ";"
#define CAT_TX_MAX_CMDS     16      // commands in one internal batch
#define USB_OUT_LEVEL_MAX   100U    // P4 of the EX item = 000 - 100
// Audio level sent to the PC, applied whenever a receiver is in DATA-USB. 100 gives the FT8 decoder the
// full-scale audio the radio can deliver (the decoder normalises the level itself).
#define DATA_USB_OUT_LEVEL  100U
#define DATA_USB_MODE_CODE  'C'


#define CAT_TWO(a, b)       ((unsigned)(((unsigned char)(a) << 8) | (unsigned char)(b)))

static bool all_digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return n > 0;
}

static unsigned digits_value(const char *s, size_t n)
{
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        v = v * 10u + (unsigned)(s[i] - '0');
    }
    return v;
}

static bool is_side(char c)
{
    return c == '0' || c == '1';
}

/** A receiver the radio has: MAIN always, SUB only on a radio with two receivers */
static bool is_rx_side(const rig_profile_t *rig, char c)
{
    return c == '0' || (c == '1' && rig->dual_rx);
}

static bool is_mode_code(const rig_profile_t *rig, char c)
{
    if ((c == 'H' || c == 'I') && !rig->c4fm) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_modes) / sizeof(s_modes[0]); i++) {
        if (s_modes[i].code == c) {
            return true;
        }
    }
    return false;
}

/** One complete command: two upper-case letters, its parameters and the closing ';' (len includes it) */
static bool cat_cmd_allowed(const char *c, size_t len)
{
    if (len < 3 || len > CAT_CMD_MAX_BYTES || c[len - 1] != ';') {
        return false;
    }
    const char *p = c + 2;          // parameters, without the ';'
    const size_t pl = len - 3;
    const rig_profile_t *rig = s_rig;
    const bool ctl = rig->can_control;
    const bool known = rig->rig != CAT_RIG_NONE;     // a radio answered ID;

    switch (CAT_TWO(c[0], c[1])) {
    case CAT_TWO('I', 'D'):         // identify (used to find the CAT port)
        return pl == 0;
    case CAT_TWO('S', 'V'):         // swap VFOs
    case CAT_TWO('A', 'B'):         // copy MAIN to SUB
    case CAT_TWO('B', 'A'):         // copy SUB to MAIN
        return ctl && pl == 0;
    case CAT_TWO('A', 'I'):         // auto information ON
        return ctl && pl == 1 && p[0] == '1';
    case CAT_TWO('R', 'I'):         // radio information (read), FTX-1
        return rig->has_ri && pl == 1 && p[0] == '0';
    case CAT_TWO('B', 'Y'):         // busy (read), FTDX10
        return rig->has_by_tx && pl == 0;
    case CAT_TWO('T', 'X'):         // TX state, READ ONLY (FTDX10). "TX" with a parameter is never allowed.
        return rig->has_by_tx && pl == 0;
    case CAT_TWO('R', 'M'):         // meter (read): the S meter item of this radio only
        return rig->meter_side != 0 && pl == 1 && p[0] == rig->meter_side;
    case CAT_TWO('P', 'S'):         // power
        return ctl && pl == 1 && is_side(p[0]);
    case CAT_TWO('F', 'A'):         // frequency, read or set within the radio's range
    case CAT_TWO('F', 'B'): {
        if (pl == 0) {
            return known;
        }
        if (!ctl || pl != 9 || !all_digits(p, 9)) {
            return false;
        }
        const unsigned hz = digits_value(p, 9);
        return hz >= rig->freq_min && hz <= rig->freq_max;
    }
    case CAT_TWO('M', 'D'):         // mode, read or set to a mode code this radio has
        if (pl == 1) {
            return known && is_side(p[0]);
        }
        return ctl && pl == 2 && is_side(p[0]) && is_mode_code(rig, p[1]);
    case CAT_TWO('A', 'G'):         // AF gain
    case CAT_TWO('S', 'Q'): {       // squelch
        if (!ctl) {
            return false;
        }
        if (pl == 1) {
            return is_rx_side(rig, p[0]);
        }
        const unsigned max = (c[0] == 'S') ? rig->sq_max : 255u;
        return pl == 4 && is_rx_side(rig, p[0]) && all_digits(p + 1, 3) && digits_value(p + 1, 3) <= max;
    }
    case CAT_TWO('E', 'X'): {       // menu: only this radio's DATA-mode audio output level, set 000 - 100
        if (rig->ex_data_out == NULL) {
            return false;
        }
        return pl == 9 && memcmp(p, rig->ex_data_out, 6) == 0 && all_digits(p + 6, 3) &&
               digits_value(p + 6, 3) <= USB_OUT_LEVEL_MAX;
    }
    case CAT_TWO('B', 'P'): {       // manual notch: kind 0 = ON/OFF, kind 1 = frequency in 10 Hz steps
        if (!ctl) {
            return false;
        }
        if (pl == 2) {
            return is_rx_side(rig, p[0]) && is_side(p[1]);
        }
        if (pl != 5 || !is_rx_side(rig, p[0]) || !is_side(p[1]) || !all_digits(p + 2, 3)) {
            return false;
        }
        const unsigned v = digits_value(p + 2, 3);
        return p[1] == '0' ? v <= 1 : (v >= 1 && v <= 320);
    }
    default:
        return false;
    }
}

/** A string made of 1..max_cmds complete commands, each on the allow-list, and nothing else */
static bool cat_cmds_allowed(const char *s, size_t max_cmds)
{
    if (s == NULL) {
        return false;
    }
    const size_t total = strnlen(s, CAT_TX_MAX_BYTES + 1);
    if (total == 0 || total > CAT_TX_MAX_BYTES) {
        return false;
    }
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i < total; i++) {
        if (s[i] == ';') {
            if (!cat_cmd_allowed(s + start, i - start + 1)) {
                return false;
            }
            count++;
            start = i + 1;
        }
    }
    return start == total && count > 0 && count <= max_cmds;
}

// ---- CAT traffic log (shown in the page's CAT LOG panel through GET /catlog) ---------------------------
#define CAT_LOG_LEN 160
static cat_log_entry_t s_log[CAT_LOG_LEN];
static uint32_t s_log_seq;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void log_add(char dir, uint8_t kind, const char *text, size_t len)
{
    cat_log_entry_t e = {.dir = dir, .kind = kind, .ms = (uint32_t)(esp_timer_get_time() / 1000)};
    if (len > CAT_LOG_TEXT_LEN - 1) {
        len = CAT_LOG_TEXT_LEN - 1;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = text[i];
        e.text[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
    }
    taskENTER_CRITICAL(&s_log_mux);
    e.seq = ++s_log_seq;
    s_log[e.seq % CAT_LOG_LEN] = e;
    taskEXIT_CRITICAL(&s_log_mux);
}

/** Log every command of a string of commands ("FA;FB;MD0;") as its own entry */
static void log_cmds(char dir, uint8_t kind, const char *cmds)
{
    size_t start = 0;
    for (size_t i = 0; cmds[i] != '\0'; i++) {
        if (cmds[i] == ';') {
            log_add(dir, kind, cmds + start, i - start + 1);
            start = i + 1;
        }
    }
    if (cmds[start] != '\0') {
        log_add(dir, kind, cmds + start, strlen(cmds + start));
    }
}

/** A periodic status answer: logged as a poll so the page can hide it */
static bool is_poll_answer(const char *pkt)
{
    return strncmp(pkt, "RM", 2) == 0 || strncmp(pkt, "RI", 2) == 0 || strncmp(pkt, "BY", 2) == 0 ||
           strncmp(pkt, "TX", 2) == 0;
}

size_t cat_log_read(uint32_t since_seq, cat_log_entry_t *out, size_t max)
{
    size_t n = 0;
    taskENTER_CRITICAL(&s_log_mux);
    const uint32_t last = s_log_seq;
    uint32_t first = (last > CAT_LOG_LEN) ? last - CAT_LOG_LEN + 1 : 1;
    if (since_seq + 1 > first) {
        first = since_seq + 1;
    }
    for (uint32_t q = first; q <= last && n < max; q++) {
        out[n++] = s_log[q % CAT_LOG_LEN];
    }
    taskEXIT_CRITICAL(&s_log_mux);
    return n;
}

/** The only place that writes to the radio */
static esp_err_t cat_tx_kind(cdc_acm_dev_hdl_t dev, const char *cmds, cat_log_kind_t kind)
{
    if (!cat_cmds_allowed(cmds, CAT_TX_MAX_CMDS)) {
        ESP_LOGE(TAG, "Blocked a CAT command that is not on the allow-list");
        if (cmds != NULL) {
            log_cmds('T', CAT_LOG_BLOCKED, cmds);
        }
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)cmds, strlen(cmds), TX_TIMEOUT_MS);
    log_cmds('T', err == ESP_OK ? kind : CAT_LOG_ERROR, cmds);
    return err;
}

static esp_err_t cat_tx(cdc_acm_dev_hdl_t dev, const char *cmds)
{
    return cat_tx_kind(dev, cmds, CAT_LOG_CMD);
}

/** Send a command and wait for the answer that starts with the same two letters */
static bool query(cdc_acm_dev_hdl_t dev, const char *cmd, char *out, size_t out_size)
{
    s_got_answer = false;
    ulTaskNotifyTake(pdTRUE, 0);
    if (cat_tx(dev, cmd) != ESP_OK) {
        return false;
    }
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ANSWER_TIMEOUT_MS);
    while ((int32_t)(end - xTaskGetTickCount()) > 0 && !s_disconnected) {
        if (s_got_answer) {
            s_got_answer = false;
            if (strncmp(s_answer, cmd, 2) == 0) {
                strlcpy(out, s_answer, out_size);
                return true;
            }
            if (strcmp(s_answer, "?;") == 0) {
                return false;          // the radio rejected the command
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    return false;
}

static void publish(bool connected, int busy, int s_main, int s_sub,
                    int64_t freq_main, int64_t freq_sub,
                    char mode_main_raw, const char *mode_main,
                    char mode_sub_raw, const char *mode_sub)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (connected) {
        s_state.connected = true;
        if (busy >= 0) {
            s_state.busy = busy;
        }
        if (s_main >= 0) {
            s_state.s_main = (uint8_t)s_main;
        }
        if (s_sub >= 0) {
            s_state.s_sub = (uint8_t)s_sub;
        }
        if (freq_main >= 0) {
            s_state.freq_main = (uint32_t)freq_main;
        }
        if (freq_sub >= 0) {
            s_state.freq_sub = (uint32_t)freq_sub;
        }
        if (mode_main_raw != 0) {
            s_state.mode_main_raw = mode_main_raw;
        }
        if (mode_main != NULL && mode_main[0] != '\0') {
            strlcpy(s_state.mode_main, mode_main, sizeof(s_state.mode_main));
        }
        if (mode_sub_raw != 0) {
            s_state.mode_sub_raw = mode_sub_raw;
        }
        if (mode_sub != NULL && mode_sub[0] != '\0') {
            strlcpy(s_state.mode_sub, mode_sub, sizeof(s_state.mode_sub));
        }
        s_last_answer_us = esp_timer_get_time();
    } else {
        s_state.connected = false;
        s_state.busy = false;
        s_state.tx = false;
        s_state.s_main = 0;
        s_state.s_sub = 0;
        s_state.freq_main = 0;
        s_state.freq_sub = 0;
        s_state.mode_main_raw = 0;
        s_state.mode_sub_raw = 0;
        s_state.mode_main[0] = '\0';
        s_state.mode_sub[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

/** Answer is "RI" P1 P2..P8 ";" (11 characters). P4 = TX state (1 = transmitting), P8, the last digit,
 *  = squelch state (1 = open, BUSY). False on a malformed answer. */
#define RI_LEN      11
#define RI_P4_INDEX 5
#define RI_P8_INDEX 9
static bool parse_ri(const char *a, bool *busy, bool *tx)
{
    if (strlen(a) != RI_LEN || strncmp(a, "RI0", 3) != 0 || a[RI_LEN - 1] != ';') {
        return false;
    }
    *busy = a[RI_P8_INDEX] == '1';
    *tx = a[RI_P4_INDEX] == '1';
    return true;
}

/** S meter, 10 characters either way (0-255):
 *  FTX-1  "RM0" + 3-digit MAIN + 3-digit SUB + ";"
 *  FTDX10 "RM1" + 3-digit MAIN + "000;"  (sub_s is set to -1: there is no SUB receiver) */
static bool parse_rm(const char *a, int *main_s, int *sub_s)
{
    if (strlen(a) != 10 || strncmp(a, "RM", 2) != 0 || (a[2] != '0' && a[2] != '1') || a[9] != ';') {
        return false;
    }
    for (int i = 3; i < 9; i++) {
        if (!isdigit((unsigned char)a[i])) {
            return false;
        }
    }
    *main_s = (a[3] - '0') * 100 + (a[4] - '0') * 10 + (a[5] - '0');
    *sub_s = (a[2] == '0') ? (a[6] - '0') * 100 + (a[7] - '0') * 10 + (a[8] - '0') : -1;
    return *main_s <= 255 && *sub_s <= 255;
}

/** FTDX10 "BY" + P1 (1 = squelch open) + P2 + ";" */
static bool parse_by(const char *a, bool *busy)
{
    if (strlen(a) != 5 || strncmp(a, "BY", 2) != 0 || !isdigit((unsigned char)a[2]) || a[4] != ';') {
        return false;
    }
    *busy = a[2] == '1';
    return true;
}

/** FTDX10 "TX" + P1 + ";": 0 = receive, 1 = CAT TX, 2 = the radio transmits */
static bool parse_tx(const char *a, bool *tx)
{
    if (strlen(a) != 4 || strncmp(a, "TX", 2) != 0 || a[2] < '0' || a[2] > '2' || a[3] != ';') {
        return false;
    }
    *tx = a[2] != '0';
    return true;
}

/** "FA" or "FB" + up to 10 digits in Hz + ";" */
static bool parse_freq(const char *a, const char *prefix, uint32_t *freq_out)
{
    size_t len = strlen(a);
    if (len < 5 || len > 13 || strncmp(a, prefix, 2) != 0 || a[len - 1] != ';') {
        return false;
    }
    uint64_t val = 0;
    for (size_t i = 2; i < len - 1; i++) {
        if (!isdigit((unsigned char)a[i])) {
            return false;
        }
        val = val * 10 + (uint64_t)(a[i] - '0');
    }
    if (val > UINT32_MAX) {
        return false;
    }
    *freq_out = (uint32_t)val;
    return true;
}

/** "MD0" or "MD1" + mode digit/char + ";" */
static bool parse_mode(const char *a, const char *prefix, char *mode_raw, char *mode_name, size_t name_size)
{
    size_t len = strlen(a);
    if (len != 5 || strncmp(a, prefix, 3) != 0 || a[len - 1] != ';') {
        return false;
    }
    char c = (char)toupper((unsigned char)a[len - 2]);
    *mode_raw = c;
    const char *name = "UNKNOWN";
    for (size_t i = 0; i < sizeof(s_modes) / sizeof(s_modes[0]); i++) {
        if (s_modes[i].code == c) {
            name = s_modes[i].name;
            break;
        }
    }
    strlcpy(mode_name, name, name_size);
    return true;
}

/** "AG0", "AG1", "SQ0", "SQ1" + 3-digit (000-255) + ";" */
static bool parse_gain(const char *a, const char *prefix, int *val_out)
{
    size_t len = strlen(a);
    if (len != 7 || strncmp(a, prefix, 3) != 0 || a[6] != ';') {
        return false;
    }
    for (int i = 3; i < 6; i++) {
        if (!isdigit((unsigned char)a[i])) {
            return false;
        }
    }
    *val_out = (a[3] - '0') * 100 + (a[4] - '0') * 10 + (a[5] - '0');
    return *val_out <= 255;
}

/** "BP" + side (0/1) + kind (0 = ON/OFF, 1 = frequency) + 3 digits + ";" */
static bool parse_notch(const char *a, int *side, int *kind, int *val)
{
    if (strlen(a) != 8 || strncmp(a, "BP", 2) != 0 || a[7] != ';') {
        return false;
    }
    if ((a[2] != '0' && a[2] != '1') || (a[3] != '0' && a[3] != '1')) {
        return false;
    }
    for (int i = 4; i < 7; i++) {
        if (!isdigit((unsigned char)a[i])) {
            return false;
        }
    }
    *side = a[2] - '0';
    *kind = a[3] - '0';
    *val = (a[4] - '0') * 100 + (a[5] - '0') * 10 + (a[6] - '0');
    return true;
}

/** Handle asynchronous CAT packet received over the stream (e.g. from Auto Information) */
static void handle_cat_packet(const char *pkt)
{
    if (!pkt || pkt[0] == '\0' || !s_lock) {
        return;
    }

    uint32_t f = 0;
    if (parse_freq(pkt, "FA", &f)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.freq_main = f;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }
    if (parse_freq(pkt, "FB", &f)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.freq_sub = f;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }

    char mode_raw = 0;
    char mode_name[16] = {0};
    if (parse_mode(pkt, "MD0", &mode_raw, mode_name, sizeof(mode_name))) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.mode_main_raw = mode_raw;
            strlcpy(s_state.mode_main, mode_name, sizeof(s_state.mode_main));
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }
    if (parse_mode(pkt, "MD1", &mode_raw, mode_name, sizeof(mode_name))) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.mode_sub_raw = mode_raw;
            strlcpy(s_state.mode_sub, mode_name, sizeof(s_state.mode_sub));
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }

    int gain_val = 0;
    int64_t now_pkt_us = esp_timer_get_time();

    if (parse_gain(pkt, "AG0", &gain_val)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Guard against radio AI echo when SUB volume was recently changed
            if (!(s_last_set_sub_vol_us > 0 && (now_pkt_us - s_last_set_sub_vol_us) < 2500000LL)) {
                s_state.vol_main = (uint8_t)gain_val;
            }
            s_state.connected = true;
            s_last_answer_us = now_pkt_us;
            xSemaphoreGive(s_lock);
        }
        return;
    }
    if (parse_gain(pkt, "AG1", &gain_val)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Guard against radio AI echo when MAIN volume was recently changed
            if (!(s_last_set_main_vol_us > 0 && (now_pkt_us - s_last_set_main_vol_us) < 2500000LL)) {
                s_state.vol_sub = (uint8_t)gain_val;
            }
            s_state.connected = true;
            s_last_answer_us = now_pkt_us;
            xSemaphoreGive(s_lock);
        }
        return;
    }

    if (parse_gain(pkt, "SQ0", &gain_val)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Guard against radio AI echo when SUB squelch was recently changed
            if (!(s_last_set_sub_sq_us > 0 && (now_pkt_us - s_last_set_sub_sq_us) < 2500000LL)) {
                s_state.sq_main = (uint8_t)gain_val;
            }
            s_state.connected = true;
            s_last_answer_us = now_pkt_us;
            xSemaphoreGive(s_lock);
        }
        return;
    }
    if (parse_gain(pkt, "SQ1", &gain_val)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Guard against radio AI echo when MAIN squelch was recently changed
            if (!(s_last_set_main_sq_us > 0 && (now_pkt_us - s_last_set_main_sq_us) < 2500000LL)) {
                s_state.sq_sub = (uint8_t)gain_val;
            }
            s_state.connected = true;
            s_last_answer_us = now_pkt_us;
            xSemaphoreGive(s_lock);
        }
        return;
    }

    int n_side = 0, n_kind = 0, n_val = 0;
    if (parse_notch(pkt, &n_side, &n_kind, &n_val)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Guard against a stale answer right after a local write
            if (!(s_last_set_notch_us[n_side] > 0 && (now_pkt_us - s_last_set_notch_us[n_side]) < 2500000LL)) {
                if (n_kind == 0) {
                    if (n_side) s_state.notch_sub = (n_val != 0); else s_state.notch_main = (n_val != 0);
                } else if (n_val >= 1 && n_val <= 320) {
                    if (n_side) s_state.notch_hz_sub = (uint16_t)(n_val * 10); else s_state.notch_hz_main = (uint16_t)(n_val * 10);
                }
            }
            s_state.connected = true;
            s_last_answer_us = now_pkt_us;
            xSemaphoreGive(s_lock);
        }
        return;
    }

    bool ri_busy = false, ri_tx = false;
    if (parse_ri(pkt, &ri_busy, &ri_tx)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.busy = ri_busy;
            s_state.tx = ri_tx;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }

    bool by_busy = false;
    if (parse_by(pkt, &by_busy)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.busy = by_busy;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }

    bool tx_on = false;
    if (parse_tx(pkt, &tx_on)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.tx = tx_on;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }

    int sm_main = 0, sm_sub = 0;
    if (parse_rm(pkt, &sm_main, &sm_sub)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.s_main = (uint8_t)sm_main;
            s_state.s_sub = sm_sub >= 0 ? (uint8_t)sm_sub : 0;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }
}

/** Copy the detected radio profile into the published state (read by the web UI through /cat) */
static void publish_profile(void)
{
    const rig_profile_t *rig = s_rig;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.rig = rig->rig;
    strlcpy(s_state.rig_name, rig->name, sizeof(s_state.rig_name));
    strlcpy(s_state.rig_id, s_rig_id, sizeof(s_state.rig_id));
    s_state.dual_rx = rig->dual_rx;
    s_state.sq_max = (uint16_t)rig->sq_max;
    s_state.freq_min = rig->freq_min;
    s_state.freq_max = rig->freq_max;
    s_state.can_control = rig->can_control;
    s_state.c4fm = rig->c4fm;
    xSemaphoreGive(s_lock);
}

/** True when a receiver is currently in DATA-USB (MAIN or SUB; on a one-receiver radio MAIN only) */
static bool data_usb_active(void)
{
    bool active = false;
    const bool dual = s_rig->dual_rx;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        active = s_state.mode_main_raw == DATA_USB_MODE_CODE ||
                 (dual && s_state.mode_sub_raw == DATA_USB_MODE_CODE);
        xSemaphoreGive(s_lock);
    }
    return active;
}

/** Send one of the profile's polled command strings, if it has one */
static void send_poll(cdc_acm_dev_hdl_t dev, const char *cmds)
{
    if (cmds != NULL) {
        cat_tx_kind(dev, cmds, CAT_LOG_POLL);
    }
}

static void poll_loop(cdc_acm_dev_hdl_t dev)
{
    const rig_profile_t *rig = s_rig;
    uint32_t poll_count = 0;
    bool usb_level_applied = false;   // DATA audio output level already set for the current DATA-USB session

    // Request the initial state asynchronously (rx_cb parses answers as they arrive). The sync string
    // starts with AI1; (auto information ON, the radio then pushes changes by itself) on the known radios.
    ESP_LOGI(TAG, "Polling %s", rig->name);
    send_poll(dev, rig->sync_all);
    vTaskDelay(pdMS_TO_TICKS(50));
    send_poll(dev, rig->poll_status);
    send_poll(dev, rig->poll_fast);

    while (!s_disconnected) {
        // 1. Process any pending commands in the queue (already checked against the allow-list when queued)
        cat_cmd_item_t item;
        while (s_cmd_queue && xQueueReceive(s_cmd_queue, &item, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Sending CAT command: %s", item.cmd);
            cat_tx(dev, item.cmd);
            vTaskDelay(pdMS_TO_TICKS(10));
            // SV (swap) / AB / BA (copy) change both VFOs: re-read them so the UI does not
            // show stale values until the next periodic sync
            if (strcmp(item.cmd, "SV;") == 0 || strcmp(item.cmd, "AB;") == 0 || strcmp(item.cmd, "BA;") == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                send_poll(dev, rig->resync_vfo);
            }
        }

        // 1b. Entering DATA-USB (from the web UI or the radio itself): set the DATA audio output level
        //     (the profile's menu item) once. Leaving DATA-USB re-arms it, so a level changed by hand
        //     stays until the next entry.
        if (rig->ex_data_out != NULL && data_usb_active()) {
            if (!usb_level_applied) {
                char cmd[16];
                snprintf(cmd, sizeof(cmd), "EX%s%03u;", rig->ex_data_out, (unsigned)DATA_USB_OUT_LEVEL);
                ESP_LOGI(TAG, "DATA-USB: setting menu %s (DATA audio output level) to %u", rig->ex_data_out,
                         (unsigned)DATA_USB_OUT_LEVEL);
                usb_level_applied = cat_tx(dev, cmd) == ESP_OK;
            }
        } else {
            usb_level_applied = false;
        }

        // 2. Poll the S meter every 100 ms
        send_poll(dev, rig->poll_fast);

        // 3. Poll BUSY / TX every 500 ms (every 5 iterations)
        if ((poll_count % 5) == 0) {
            send_poll(dev, rig->poll_status);
        }

        // 4. Periodic slow refresh / sync every 5 seconds (50 iterations)
        if ((poll_count % 50) == 0) {
            send_poll(dev, rig->sync_all);
        }
        poll_count++;

        // 5. Check if port is alive (no bytes received for > 10 seconds)
        int64_t now_us = esp_timer_get_time();
        if (s_last_answer_us > 0 && (now_us - s_last_answer_us) > 10000000LL) {
            ESP_LOGW(TAG, "No CAT answer for 10s, reconnecting");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/** Enable the UART, set its speed and line format, and keep DTR/RTS low (the radio may use them for PTT) */
static esp_err_t cp210x_setup(cdc_acm_dev_hdl_t dev, uint8_t intf, uint32_t baud)
{
    ESP_RETURN_ON_ERROR(cdc_acm_host_send_custom_request(dev, CP210X_REQ_OUT, CP210X_IFC_ENABLE, 1, intf, 0, NULL),
                        TAG, "enable");
    ESP_RETURN_ON_ERROR(cdc_acm_host_send_custom_request(dev, CP210X_REQ_OUT, CP210X_SET_BAUDRATE, 0, intf,
                                                         sizeof(baud), (uint8_t *)&baud), TAG, "baud");
    ESP_RETURN_ON_ERROR(cdc_acm_host_send_custom_request(dev, CP210X_REQ_OUT, CP210X_SET_LINE_CTL, CP210X_LINE_8N1,
                                                         intf, 0, NULL), TAG, "line");
    return cdc_acm_host_send_custom_request(dev, CP210X_REQ_OUT, CP210X_SET_MHS, CP210X_MHS_DTR_RTS_LOW, intf, 0, NULL);
}

/** Open the CP210x at USB address addr and return true if it answers ID; as a CAT port */
static bool try_port(uint8_t addr, cdc_acm_dev_hdl_t *dev, uint8_t *opened)
{
    const cdc_acm_host_open_config_t cfg = {
        .vid = SILABS_VID,
        .pid = CDC_HOST_ANY_PID,
        .interface_idx = 0,
        .dev_addr = addr,
        .connection_timeout_ms = 500,
        .out_buffer_size = 64,
        .in_buffer_size = 0,
        .event_cb = event_cb,
        .data_cb = rx_cb,
        .user_arg = NULL,
    };
    esp_err_t err = cdc_acm_host_open(&cfg, dev);
    if (err != ESP_OK) {
        diag("open addr %u: %s", addr, esp_err_to_name(err));
        return false;
    }
    (*opened)++;
    // The CP210x UARTs here are single-interface devices (the CDC driver opened interface 0)
    const uint8_t intf = cfg.interface_idx;
    // CAT_BAUD first (the configured CAT-1 rate), then the other rates the radio offers
    static const uint32_t bauds[] = {CAT_BAUD, 38400, 4800, 9600, 19200};
    for (size_t b = 0; b < sizeof(bauds) / sizeof(bauds[0]); b++) {
        err = cp210x_setup(*dev, intf, bauds[b]);
        if (err != ESP_OK) {
            diag("setup addr %u: %s", addr, esp_err_to_name(err));
            break;
        }
        char a[RX_BUF_BYTES];
        s_rx_len = 0;
        s_rx_bytes = 0;
        memset(s_rx_tail, ' ', sizeof(s_rx_tail) - 1);
        s_rx_tail[sizeof(s_rx_tail) - 1] = '\0';
        for (int i = 0; i < 2; i++) {
            if (query(*dev, "ID;", a, sizeof(a))) {
                // "ID" + 4 characters + ";". Anything else still marks the CAT port, with the read-only profile.
                const bool well_formed = strlen(a) == 7 && a[6] == ';';
                memset(s_rig_id, 0, sizeof(s_rig_id));
                if (well_formed) {
                    for (int k = 0; k < 4; k++) {
                        s_rig_id[k] = isalnum((unsigned char)a[2 + k]) ? a[2 + k] : '?';
                    }
                }
                s_rig = well_formed ? profile_for_id(s_rig_id) : &s_profile_unknown;
                ESP_LOGI(TAG, "CAT port at USB address %u, interface %u, %lu bps: %s -> %s", addr, intf,
                         (unsigned long)bauds[b], a, s_rig->name);
                return true;
            }
        }
        ESP_LOGI(TAG, "addr %u @%lu: no ID; answer (%lu bytes received)", addr, (unsigned long)bauds[b],
                 (unsigned long)s_rx_bytes);
        diag("addr %u: no answer, rx %lu B [%s]", addr, (unsigned long)s_rx_bytes, s_rx_tail);
    }
    cdc_acm_host_close(*dev);
    *dev = NULL;
    return false;
}

static cdc_acm_dev_hdl_t find_cat_port(void)
{
    uint8_t addrs[10];
    int count = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &count) != ESP_OK) {
        return NULL;
    }
    uint8_t opened = 0;
    diag_counts((uint8_t)count, 0, 0);
    if (count == 0) {
        diag("no USB device on the bus");
    }
    for (int i = 0; i < count; i++) {
        cdc_acm_dev_hdl_t dev = NULL;
        if (try_port(addrs[i], &dev, &opened)) {
            diag_counts((uint8_t)count, opened, 1);
            diag("%s", "");
            return dev;
        }
        diag_counts((uint8_t)count, opened, 0);
    }
    return NULL;
}

static void cat_task(void *arg)
{
    s_task = xTaskGetCurrentTaskHandle();
    for (;;) {
        s_disconnected = false;
        s_rig = &s_profile_none;          // only ID; may be sent until a radio has answered
        memset(s_rig_id, 0, sizeof(s_rig_id));
        cdc_acm_dev_hdl_t dev = find_cat_port();
        if (dev) {
            publish_profile();
            publish(true, -1, -1, -1, -1, -1, 0, NULL, 0, NULL);
            poll_loop(dev);
            ESP_LOGW(TAG, "CAT port lost");
            cdc_acm_host_close(dev);
            publish(false, 0, 0, 0, 0, 0, 0, "", 0, "");
            s_rig = &s_profile_none;
            memset(s_rig_id, 0, sizeof(s_rig_id));
            publish_profile();
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
    }
}

esp_err_t cat_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    s_cmd_queue = xQueueCreate(10, sizeof(cat_cmd_item_t));
    ESP_RETURN_ON_FALSE(s_cmd_queue, ESP_ERR_NO_MEM, TAG, "no queue memory");
    const cdc_acm_host_driver_config_t cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 0,
        .new_dev_cb = NULL,
    };
    ESP_RETURN_ON_ERROR(cdc_acm_host_install(&cfg), TAG, "cdc_acm install");
    BaseType_t ok = xTaskCreatePinnedToCore(cat_task, "cat", 4096, NULL, CAT_TASK_PRIORITY, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "cat task");
    return ESP_OK;
}

void cat_get_state(cat_state_t *state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *state = s_state;
    state->age_ms = s_state.connected ? (uint32_t)((esp_timer_get_time() - s_last_answer_us) / 1000) : 0;
    xSemaphoreGive(s_lock);
}

/** Queue exactly one allow-listed command (with its ';'). Private on purpose: callers use the typed functions. */
static esp_err_t cat_enqueue(const char *cmd)
{
    if (!cat_cmds_allowed(cmd, 1)) {
        ESP_LOGE(TAG, "Refused a CAT command that is not on the allow-list");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    cat_cmd_item_t item;
    memset(&item, 0, sizeof(item));
    strlcpy(item.cmd, cmd, sizeof(item.cmd));

    if (xQueueSend(s_cmd_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

esp_err_t cat_swap_vfo(void)
{
    return cat_enqueue("SV;");
}

esp_err_t cat_copy_main_to_sub(void)
{
    return cat_enqueue("AB;");
}

esp_err_t cat_copy_sub_to_main(void)
{
    return cat_enqueue("BA;");
}

esp_err_t cat_set_freq(bool sub, uint32_t freq_hz)
{
    const rig_profile_t *rig = s_rig;
    if (!rig->can_control) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz < rig->freq_min || freq_hz > rig->freq_max) {
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[32];
    // FA / FB command: 9-digit Hz padded with leading zeros
    snprintf(cmd, sizeof(cmd), "%s%09lu;", sub ? "FB" : "FA", (unsigned long)freq_hz);
    esp_err_t err = cat_enqueue(cmd);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sub) {
            s_state.freq_sub = freq_hz;
        } else {
            s_state.freq_main = freq_hz;
        }
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t cat_set_mode_char(bool sub, char mode_char)
{
    char code = (char)toupper((unsigned char)mode_char);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "MD%c%c;", sub ? '1' : '0', code);
    esp_err_t err = cat_enqueue(cmd);
    if (err == ESP_OK) {
        const char *name = "UNKNOWN";
        for (size_t i = 0; i < sizeof(s_modes) / sizeof(s_modes[0]); i++) {
            if (s_modes[i].code == code) {
                name = s_modes[i].name;
                break;
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sub) {
            s_state.mode_sub_raw = code;
            strlcpy(s_state.mode_sub, name, sizeof(s_state.mode_sub));
        } else {
            s_state.mode_main_raw = code;
            strlcpy(s_state.mode_main, name, sizeof(s_state.mode_main));
        }
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t cat_set_mode(bool sub, const char *mode_name)
{
    if (!mode_name) {
        return ESP_ERR_INVALID_ARG;
    }
    // If a single char was passed (e.g. "2" or "A")
    if (strlen(mode_name) == 1) {
        return cat_set_mode_char(sub, mode_name[0]);
    }
    for (size_t i = 0; i < sizeof(s_modes) / sizeof(s_modes[0]); i++) {
        if (strcasecmp(s_modes[i].name, mode_name) == 0) {
            return cat_set_mode_char(sub, s_modes[i].code);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cat_set_volume(bool sub, uint8_t volume)
{
    if (sub && !s_rig->dual_rx) {
        return ESP_ERR_NOT_SUPPORTED;     // no SUB receiver
    }
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "AG%c%03u;", sub ? '1' : '0', volume);
    esp_err_t err = cat_enqueue(cmd);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sub) {
            s_state.vol_sub = volume;
            s_last_set_sub_vol_us = esp_timer_get_time();
        } else {
            s_state.vol_main = volume;
            s_last_set_main_vol_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t cat_set_notch(bool sub, bool on)
{
    if (sub && !s_rig->dual_rx) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "BP%c0%03u;", sub ? '1' : '0', on ? 1u : 0u);
    esp_err_t err = cat_enqueue(cmd);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sub) s_state.notch_sub = on; else s_state.notch_main = on;
        s_last_set_notch_us[sub ? 1 : 0] = esp_timer_get_time();
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t cat_set_notch_freq(bool sub, uint16_t hz)
{
    if (hz < 10 || hz > 3200) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sub && !s_rig->dual_rx) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    unsigned steps = (hz + 5u) / 10u;       // the radio works in 10 Hz steps
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "BP%c1%03u;", sub ? '1' : '0', steps);
    esp_err_t err = cat_enqueue(cmd);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sub) s_state.notch_hz_sub = (uint16_t)(steps * 10); else s_state.notch_hz_main = (uint16_t)(steps * 10);
        s_last_set_notch_us[sub ? 1 : 0] = esp_timer_get_time();
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t cat_set_power(bool on)
{
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "PS%u;", on ? 1u : 0u);
    return cat_enqueue(cmd);
}

esp_err_t cat_set_squelch(bool sub, uint8_t squelch)
{
    const rig_profile_t *rig = s_rig;
    if (sub && !rig->dual_rx) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (squelch > rig->sq_max) {
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "SQ%c%03u;", sub ? '1' : '0', squelch);
    esp_err_t err = cat_enqueue(cmd);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sub) {
            s_state.sq_sub = squelch;
            s_last_set_sub_sq_us = esp_timer_get_time();
        } else {
            s_state.sq_main = squelch;
            s_last_set_main_sq_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
    }
    return err;
}
