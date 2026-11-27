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
 * All writes use the CAT allow-list. Discovery also permits one standalone parser delimiter.
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
#include <stdatomic.h>
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
#include "rigctl_protocol.h"
#include "app_config.h"
#include "nvs.h"
#include "usb_audio.h"

// The FTX-1 exposes two CP210x UARTs (Silicon Labs VID 0x10C4). Both have the same VID/PID, so they are
// opened by USB device address. Whichever returns a valid ID at a supported speed is used as the CAT port.
#define SILABS_VID          0x10C4
#define CAT_SETTLE_US       3000000LL
#define CAT_BAUD_SETTLE_MS  50
#define CAT_DELIMITER_MS    60
#define CAT_CACHED_SCAN_MS  1000
#define CAT_CACHED_ONLY_US  10000000LL
#define CAT_RECOVER_US      30000000LL
#define CAT_RECOVER_MAX     3
#define NORMAL_ANSWER_MS    100
#define CAT_CACHE_NVS       "cat_link"
#define CAT_CACHE_BAUD      "last_baud"
#define CAT_TASK_PRIORITY   6         // above HTTPS (5): voice frames must not delay CAT safety polling
#define POLL_PERIOD_MS      100
#define RADIO_STATUS_MAX_AGE_US 1500000LL  // BUSY/TX is polled every five iterations
#define MOD_MENU_MAX_AGE_US 15000000LL
#define ANSWER_TIMEOUT_MS   300
#define TX_TIMEOUT_MS       200
#define FTX1_KEYED_WRITE_TIMEOUT_MS 1200
#define FTDX10_KEYED_WRITE_TIMEOUT_MS 300
#define FTX1_KEYED_REPLY_TIMEOUT_US 1500000LL
#define FTDX10_KEYED_REPLY_TIMEOUT_US 300000LL
#define KEYED_CAT_MAX_AGE_US   3000000LL
#define FTDX10_HI_SWR_STARTUP_US 300000LL
#define RX_BUF_BYTES        64
#define MAX_FAILS           10       // consecutive missing answers before the port is dropped
#define SCAN_PERIOD_MS      2000
#define SM_RELEASE          0.25f    // S meter fall per 100 ms reading: about 0.35 s to settle

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
static SemaphoreHandle_t s_wire_lock; // orders CAT writes against a USB recovery reset
static SemaphoreHandle_t s_ptt_lock;  // serializes PTT writes with closing the CAT port
static cdc_acm_dev_hdl_t s_ptt_port;
static bool s_ptt_ready;
static portMUX_TYPE s_usb_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_usb_seen_us[128];
static int64_t s_usb_reset_until;
static bool s_usb_resetting;
static atomic_uchar s_cat_addr;
static usb_host_client_handle_t s_usb_observer;
static atomic_uint s_cat_session;
static unsigned s_next_cat_session;    // assigned by the CAT task
static int64_t s_cat_missing_since;
static unsigned s_cat_recoveries;
static uint8_t s_scan_after_addr;       // rotate after the first port that exhausted its baud probes
static atomic_bool s_link_starting;
static uint32_t s_last_baud;
static const uint32_t s_default_bauds[] = {115200, 38400, 19200, 9600, 4800};
static int64_t s_swr_status_us;
static int64_t s_hi_swr_us;
static int64_t s_normal_swr_us;
static int64_t s_rm6_reply_us;
static bool s_unkey_requested;
static cat_tx_meters_t s_tx_meters;
static atomic_bool s_ft8_alc;

#define ALC_QUERY_MS 40
#define ALC_PERIOD_US 500000
static cat_write_stats_t s_write_stats;
static int64_t s_tx_vfo_status_us;
static int64_t s_mode_status_us[2];
static int64_t s_ssb_menu_us[2];
static char s_ssb_menu[2];
static int64_t s_data_menu_us[2];
static char s_data_menu[2];
static char s_reported_mode[2];
static int64_t s_last_ptt_status_us;
static char s_ptt_mode;
static int64_t s_last_answer_us;
static int64_t s_last_tx_status_us;     // protected by s_lock
static int64_t s_last_set_main_sq_us;
static int64_t s_last_set_sub_sq_us;
static int64_t s_last_set_notch_us[2];       // [MAIN, SUB]: last local BP write (ON/OFF or frequency)
static int64_t s_last_set_main_vol_us;
static int64_t s_last_set_sub_vol_us;
static atomic_uint s_rigctl_results[2];  // one completion slot per TCP client
static QueueHandle_t s_cmd_queue;      // queued commands to send over CAT

typedef struct {
    char cmd[32];
    unsigned ticket;
    unsigned session;
    int64_t expires_us;
    bool sub;
    bool require_active;
    cat_rig_t rig;
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
    bool can_tx;
    const char *ex_read[10];
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
        .can_tx = true, .ex_read = {"010413", "010414", "010113", "010114", "010213", "010214", "010312", "010313"},
        .dual_rx = true, .can_control = true, .c4fm = true, .has_ri = true, .has_by_tx = false,
        .meter_side = '0', .sq_max = 255,
        .ex_data_out = "010411",      // RADIO SETTING / MODE DATA / USB OUT LEVEL
        .poll_fast = "RM0;",
        .poll_status = "RI0;TX;",
        .sync_all = "FA;FB;MD0;MD1;AG0;AG1;SQ0;SQ1;BP00;BP01;BP10;BP11;",
        .resync_vfo = "FA;FB;MD0;MD1;",
    },
    {
        .rig = CAT_RIG_FTDX10, .name = "FTDX10", .id = "0761",
        .freq_min = 30000U, .freq_max = 75000000U,
        .can_tx = true, .ex_read = {"010415", "010416", "010417", "010113", "010114", "010115", "010213", "010215", "010216"},
        .dual_rx = false, .can_control = true, .c4fm = false, .has_ri = false, .has_by_tx = true,
        .meter_side = '1', .sq_max = 100,
        .ex_data_out = "010413",      // RADIO SETTING / MODE PSK/DATA / DATA OUT LEVEL
        .poll_fast = "RM1;",
        .poll_status = "BY;TX;",
        .sync_all = "FA;FB;MD0;MD1;AG0;SQ0;BP00;BP01;",
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
static portMUX_TYPE s_query_mux = portMUX_INITIALIZER_UNLOCKED;
typedef enum { QUERY_TIMEOUT, QUERY_ANSWER, QUERY_REJECTED, QUERY_FAILED } query_result_t;
static struct {
    char command[13];
    char answer[RX_BUF_BYTES];
    query_result_t result;
    bool waiting;
    bool transition_reject;
} s_query;
#define CAT_REJECTED_POLLS 48
#define CAT_REJECT_LIMIT 3
#define CAT_REJECT_RETRY_US 60000000LL
#define CAT_TRANSITION_GRACE_MS 1000u
static struct {
    char cmd[13];
    unsigned streak;
    int64_t retry_us;
} s_rejected_polls[CAT_REJECTED_POLLS];
static atomic_uint s_transition_ms;
static atomic_uint s_effective_rejects;
static size_t s_rejected_count;
static bool s_split_keyed_polls;
static unsigned s_keyed_reject_baseline;
static atomic_uint s_rejects;
static atomic_bool s_rx_restart;
static volatile uint32_t s_rx_bytes;     // all bytes received from the open port (diagnostics)
static char s_rx_tail[17];               // last received bytes, printable only
static atomic_bool s_disconnected;

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
static bool all_digits(const char *s, size_t n);

static bool matches_query(const char *pkt, size_t length, const char *command)
{
    const size_t prefix = strlen(command) - 1;
    if (length <= prefix || memcmp(pkt, command, prefix) != 0) return false;
    if (strcmp(command, "ID;") == 0) return length == 7 && all_digits(pkt + 2, 4);
    // Even an unexpected AI value is a reply, distinct from silence.
    if (strcmp(command, "AI;") == 0) return true;
    return length > prefix + 1;
}

static void record_tx_transition(void)
{
    atomic_store(&s_transition_ms, (unsigned)(esp_timer_get_time() / 1000));
}

static bool in_tx_transition(void)
{
    const unsigned since = atomic_load(&s_transition_ms);
    return since && (unsigned)(esp_timer_get_time() / 1000) - since < CAT_TRANSITION_GRACE_MS;
}

static void record_query_answer(const char *pkt, size_t length)
{
    const bool rejected = length == 2 && memcmp(pkt, "?;", 2) == 0;
    const bool transient = rejected && in_tx_transition();
    if (rejected) {
        atomic_fetch_add(&s_rejects, 1);
        if (!transient) atomic_fetch_add(&s_effective_rejects, 1);
    }
    taskENTER_CRITICAL(&s_query_mux);
    // A trailing rejection of the standalone delimiter must not terminate an ID probe.
    if (s_query.waiting && ((rejected && strcmp(s_query.command, "ID;") != 0) ||
                            matches_query(pkt, length, s_query.command))) {
        memcpy(s_query.answer, pkt, length + 1);
        s_query.result = rejected ? QUERY_REJECTED : QUERY_ANSWER;
        s_query.transition_reject = transient;
        s_query.waiting = false;
    }
    taskEXIT_CRITICAL(&s_query_mux);
}

unsigned cat_rejected_commands(void)
{
    return atomic_load(&s_rejects);
}

static bool rx_cb(const uint8_t *data, size_t len, void *arg)
{
    if ((uintptr_t)arg != atomic_load(&s_cat_session)) return true;
    if (atomic_exchange(&s_rx_restart, false)) s_rx_len = 0;
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
            handle_cat_packet(s_rx);
            record_query_answer(s_rx, s_rx_len);
            s_rx_len = 0;
            if (s_task) {
                xTaskNotifyGive(s_task);
            }
        }
    }
    return true;
}

static void event_cb(const cdc_acm_host_dev_event_data_t *event, void *arg)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED &&
        (uintptr_t)arg == atomic_load(&s_cat_session)) {
        s_disconnected = true;
        atomic_store(&s_rx_restart, true);
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
}

static void cat_usb_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    const bool added = event->event == USB_HOST_CLIENT_EVENT_NEW_DEV;
    if (!added && event->event != USB_HOST_CLIENT_EVENT_DEV_REMOVED) return;
    const unsigned addr = added ? event->new_dev.address : event->dev_removed.address;
    if (!addr || addr >= 128) return;

    const int64_t until = added ? esp_timer_get_time() + CAT_SETTLE_US : 0;
    taskENTER_CRITICAL(&s_usb_mux);
    s_usb_seen_us[addr] = until;
    if (!added && addr == atomic_load(&s_cat_addr)) s_disconnected = true;
    taskEXIT_CRITICAL(&s_usb_mux);
}

static void observe_usb_events(void)
{
    // Only the CAT task dispatches this client's events. The CDC driver's
    // NEW_DEV callback is disabled so it cannot race a CDC port open.
    if (s_usb_observer) usb_host_client_handle_events(s_usb_observer, 0);
}

// Call around the root-port power cycle. Serialize with the USB write itself
// so a probe cannot slip out after recovery has begun.
void cat_usb_reset_begin(void)
{
    if (s_wire_lock) xSemaphoreTake(s_wire_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_usb_mux);
    s_usb_resetting = true;
    memset(s_usb_seen_us, 0, sizeof(s_usb_seen_us));
    taskEXIT_CRITICAL(&s_usb_mux);
    if (s_wire_lock) xSemaphoreGive(s_wire_lock);
}

void cat_usb_reset_end(void)
{
    taskENTER_CRITICAL(&s_usb_mux);
    s_usb_reset_until = esp_timer_get_time() + CAT_SETTLE_US;
    s_usb_resetting = false;
    taskEXIT_CRITICAL(&s_usb_mux);
}

static bool cat_usb_address_ready(unsigned addr)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_usb_mux);
    const int64_t until = addr ? s_usb_seen_us[addr] : 0;
    const bool ready = !s_usb_resetting && now >= s_usb_reset_until &&
                       (!addr || (until && now >= until));
    taskEXIT_CRITICAL(&s_usb_mux);
    return ready;
}

static bool cat_usb_ready(void)
{
    return cat_usb_address_ready(atomic_load(&s_cat_addr));
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
    case CAT_TWO('A', 'I'):         // read or enable auto information
        return ctl && (pl == 0 || (pl == 1 && p[0] == '1'));
    case CAT_TWO('R', 'I'):         // radio information (read), including Hi-SWR
        return rig->can_tx && pl == 1 && p[0] == '0';
    case CAT_TWO('B', 'Y'):         // busy (read), FTDX10
        return rig->has_by_tx && pl == 0;
    case CAT_TWO('F', 'T'):         // transmitter selection (does not key it)
        return rig->can_tx && (pl == 0 || (pl == 1 &&
            (rig->dual_rx ? is_side(p[0]) : (p[0] == '2' || p[0] == '3'))));
    case CAT_TWO('V', 'S'):         // active VFO
        return ctl && (pl == 0 || (pl == 1 && is_side(p[0])));
    case CAT_TWO('S', 'H'):         // receiver filter bandwidth
        return ctl && (pl == 1 || pl == 4) && is_rx_side(rig, p[0]) &&
               (pl == 1 || (p[1] == '0' && all_digits(p + 2, 2) && digits_value(p + 2, 2) <= 23));
    case CAT_TWO('T', 'X'):
        if (pl == 1 && p[0] == '0') {
            return true;   // emergency unkey, independent of PTT authorization or profile
        }
        return rig->can_tx && (pl == 0 || (pl == 1 && p[0] == '1' && app_config_tx_enabled()));
    case CAT_TWO('R', 'M'):         // meter (read): the S meter item of this radio only
        return pl == 1 && ((rig->meter_side && p[0] == rig->meter_side) ||
                           (rig->can_tx && p[0] >= '4' && p[0] <= '6'));
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
    case CAT_TWO('E', 'X'):
        if (pl == 6) {
            for (size_t i = 0; i < sizeof(rig->ex_read) / sizeof(rig->ex_read[0]) && rig->ex_read[i]; ++i) {
                if (memcmp(p, rig->ex_read[i], 6) == 0) {
                    return true;
                }
            }
            return false;
        } {       // menu: only this radio's DATA-mode audio output level, set 000 - 100
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
           strncmp(pkt, "TX", 2) == 0 || strncmp(pkt, "FT", 2) == 0;
}

size_t cat_log_read(uint32_t since_seq, cat_log_entry_t *out, size_t max)
{
    size_t n = 0;
    taskENTER_CRITICAL(&s_log_mux);
    const uint32_t last = s_log_seq;
    if (since_seq > last) {
        since_seq = 0;  // the browser kept its cursor across a board restart
    }
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

size_t cat_log_recent_tx_meters(uint32_t through_ms, cat_log_entry_t *out, size_t max)
{
    size_t n = 0;
    taskENTER_CRITICAL(&s_log_mux);
    const uint32_t last = s_log_seq;
    const uint32_t available = last < CAT_LOG_LEN ? last : CAT_LOG_LEN;
    for (uint32_t i = 0; n < max && i < available; ++i) {
        const cat_log_entry_t *entry = &s_log[(last - i) % CAT_LOG_LEN];
        if ((int32_t)(entry->ms - through_ms) <= 0 && entry->text[0] == 'R' && entry->text[1] == 'M' &&
            (entry->text[2] == '5' || entry->text[2] == '6')) {
            out[n++] = *entry;
        }
    }
    taskEXIT_CRITICAL(&s_log_mux);
    return n;
}

static void capture_write_timing(cat_write_timing_t *stats, unsigned us, esp_err_t err, unsigned budget_ms)
{
    ++stats->calls;
    stats->timeouts += err == ESP_ERR_TIMEOUT;
    stats->over_budget += us > budget_ms * 1000;
    if (us > stats->max_us) {
        stats->max_us = us;
    }
}

static unsigned keyed_write_timeout_ms(void)
{
    return s_rig->rig == CAT_RIG_FTDX10 ? FTDX10_KEYED_WRITE_TIMEOUT_MS : FTX1_KEYED_WRITE_TIMEOUT_MS;
}

/** The only place that writes to the radio */
static esp_err_t cat_tx_kind(cdc_acm_dev_hdl_t dev, const char *cmds, cat_log_kind_t kind, bool keyed_poll)
{
    const bool discovery_delimiter = kind == CAT_LOG_POLL && !keyed_poll &&
        cmds && strcmp(cmds, ";") == 0 && s_rig == &s_profile_none &&
        atomic_load(&s_link_starting) && atomic_load(&s_cat_session);
    if (!discovery_delimiter && !cat_cmds_allowed(cmds, CAT_TX_MAX_CMDS)) {
        ESP_LOGE(TAG, "Blocked a CAT command that is not on the allow-list");
        if (cmds != NULL) {
            log_cmds('T', CAT_LOG_BLOCKED, cmds);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wire_lock) xSemaphoreTake(s_wire_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool keyed = s_state.tx_cat;
    const bool blocked = (keyed_poll && (!keyed || s_unkey_requested)) ||
                         (keyed && !keyed_poll && strcmp(cmds, "TX0;") != 0);
    xSemaphoreGive(s_lock);
    if (blocked || s_disconnected || !cat_usb_ready()) {
        if (s_wire_lock) xSemaphoreGive(s_wire_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const unsigned budget_ms = !keyed_poll ? TX_TIMEOUT_MS :
        strcmp(cmds, "RM4;") == 0 ? ALC_QUERY_MS : keyed_write_timeout_ms();
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)cmds, strlen(cmds), budget_ms);
    const unsigned elapsed = (unsigned)(esp_timer_get_time() - start);
    if (s_wire_lock) xSemaphoreGive(s_wire_lock);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    capture_write_timing(&s_write_stats.all, elapsed, err, budget_ms);
    if (keyed) {
        capture_write_timing(&s_write_stats.keyed, elapsed, err, budget_ms);
    }
    if (strcmp(cmds, "RM5;RM6;") == 0 || strcmp(cmds, "RM5;") == 0 || strcmp(cmds, "RM6;") == 0) {
        capture_write_timing(&s_write_stats.meters, elapsed, err, budget_ms);
    }
    xSemaphoreGive(s_lock);
    log_cmds('T', err == ESP_OK ? kind : CAT_LOG_ERROR, cmds);
    return err;
}

static esp_err_t cat_tx(cdc_acm_dev_hdl_t dev, const char *cmds)
{
    return cat_tx_kind(dev, cmds, CAT_LOG_CMD, false);
}

static bool essential_poll(const char *cmd)
{
    if (strncmp(cmd, "EX", 2) == 0) return true;
    static const char *const essential[] = {
        "ID;", "TX;", "RI0;", "BY;", "RM5;", "RM6;", "FT;", "MD0;", "MD1;", "FA;", "FB;"
    };
    for (size_t i = 0; i < sizeof(essential) / sizeof(essential[0]); ++i) {
        if (strcmp(cmd, essential[i]) == 0) return true;
    }
    return false;
}

static size_t rejected_poll_index(const char *cmd)
{
    for (size_t i = 0; i < s_rejected_count; ++i) {
        if (strcmp(s_rejected_polls[i].cmd, cmd) == 0) return i;
    }
    return s_rejected_count;
}

static bool poll_rejected(const char *cmd)
{
    const size_t i = rejected_poll_index(cmd);
    return i < s_rejected_count && s_rejected_polls[i].retry_us > esp_timer_get_time();
}

static void update_poll_rejections(const char *cmd, query_result_t result, bool transient)
{
    if (essential_poll(cmd) || transient) return;
    const size_t i = rejected_poll_index(cmd);
    if (result != QUERY_REJECTED) {
        if (i < s_rejected_count) s_rejected_polls[i] = s_rejected_polls[--s_rejected_count];
        return;
    }
    if (i == s_rejected_count) {
        if (i == CAT_REJECTED_POLLS) return;
        ++s_rejected_count;
        s_rejected_polls[i].streak = 0;
        s_rejected_polls[i].retry_us = 0;
        strlcpy(s_rejected_polls[i].cmd, cmd, sizeof(s_rejected_polls[i].cmd));
    }
    if (s_rejected_polls[i].streak < CAT_REJECT_LIMIT) ++s_rejected_polls[i].streak;
    if (s_rejected_polls[i].streak == CAT_REJECT_LIMIT) {
        s_rejected_polls[i].retry_us = esp_timer_get_time() + CAT_REJECT_RETRY_US;
        ESP_LOGW(TAG, "Repeated CAT rejection of %s, retrying in 60 s", cmd);
    }
}

/** One outstanding read at a time; consume only its matching response or a rejection. */
static query_result_t query(cdc_acm_dev_hdl_t dev, const char *cmd, char *out, size_t out_size,
                            unsigned wait_ms, bool keyed)
{
    taskENTER_CRITICAL(&s_query_mux);
    strlcpy(s_query.command, cmd, sizeof(s_query.command));
    s_query.result = QUERY_TIMEOUT;
    s_query.transition_reject = false;
    s_query.waiting = true;
    taskEXIT_CRITICAL(&s_query_mux);
    ulTaskNotifyTake(pdTRUE, 0);
    const bool sent = cat_tx_kind(dev, cmd, CAT_LOG_POLL, keyed) == ESP_OK;
    if (sent) {
        const int64_t deadline = esp_timer_get_time() + wait_ms * 1000LL;
        for (;;) {
            taskENTER_CRITICAL(&s_query_mux);
            const bool pending = s_query.waiting;
            taskEXIT_CRITICAL(&s_query_mux);
            if (!pending || s_disconnected || !cat_usb_ready() || esp_timer_get_time() >= deadline) break;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        }
    }
    const bool link_ok = sent && !s_disconnected && cat_usb_ready();
    taskENTER_CRITICAL(&s_query_mux);
    const query_result_t result = link_ok ? s_query.result : QUERY_FAILED;
    if (result == QUERY_ANSWER && out) strlcpy(out, s_query.answer, out_size);
    const bool transient = s_query.transition_reject;
    s_query.waiting = false;
    taskEXIT_CRITICAL(&s_query_mux);
    update_poll_rejections(cmd, result, transient);
    return result;
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
        atomic_store(&s_transition_ms, 0);
        s_last_tx_status_us = 0;
        s_swr_status_us = 0;
        s_hi_swr_us = 0;
        s_normal_swr_us = 0;
        s_rm6_reply_us = 0;
        s_unkey_requested = false;
        s_tx_meters = (cat_tx_meters_t){0};
        atomic_store(&s_ft8_alc, false);
        s_tx_vfo_status_us = 0;
        memset(s_mode_status_us, 0, sizeof(s_mode_status_us));
        memset(s_ssb_menu_us, 0, sizeof(s_ssb_menu_us));
        memset(s_data_menu_us, 0, sizeof(s_data_menu_us));
        s_state.tx_vfo = CAT_TX_VFO_UNKNOWN;
        s_state.rx_vfo_known = false;
        s_state.rx_vfo_sub = false;
        s_state.width_code[0] = s_state.width_code[1] = -1;
        s_last_ptt_status_us = 0;
        s_ptt_mode = 0;
        s_state.tx_cat = false;
        s_state.s_main = 0;
        s_state.s_sub = 0;
        s_state.freq_main = 0;
        s_state.freq_sub = 0;
        s_state.mode_main_raw = 0;
        s_state.mode_sub_raw = 0;
        s_state.mode_main[0] = '\0';
        s_state.mode_sub[0] = '\0';
        s_state.vol_main = s_state.vol_sub = 0;
        s_state.sq_main = s_state.sq_sub = 0;
        s_state.notch_main = s_state.notch_sub = false;
        s_state.notch_hz_main = s_state.notch_hz_sub = 0;
        s_last_answer_us = 0;
        s_last_set_main_sq_us = s_last_set_sub_sq_us = 0;
        s_last_set_main_vol_us = s_last_set_sub_vol_us = 0;
        memset(s_last_set_notch_us, 0, sizeof(s_last_set_notch_us));
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
    if (strlen(a) != RI_LEN || strncmp(a, "RI0", 3) != 0 || a[RI_LEN - 1] != ';' ||
            a[RI_P4_INDEX] < '0' || a[RI_P4_INDEX] > '2') {
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
            s_reported_mode[0] = mode_raw;
            s_mode_status_us[0] = esp_timer_get_time();
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
            s_reported_mode[1] = mode_raw;
            s_mode_status_us[1] = esp_timer_get_time();
            strlcpy(s_state.mode_sub, mode_name, sizeof(s_state.mode_sub));
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        return;
    }

    int gain_val = 0;
    int64_t now_pkt_us = esp_timer_get_time();

    // Only consume the single-digit modulation menu readbacks.
    if (strlen(pkt) == 10 && strncmp(pkt, "EX010", 5) == 0 && pkt[9] == ';') {
        const bool rear_select = s_rig->rig == CAT_RIG_FTDX10;
        const bool supported = rear_select || s_rig->rig == CAT_RIG_FTX1;
        const bool data = pkt[5] == '4';
        const char source = data && rear_select ? '5' : '3';
        const int item = pkt[6] != '1' || !supported || (pkt[5] != '1' && !data) ? -1
                       : pkt[7] == source ? 0 : rear_select && pkt[7] == source + 1 ? 1 : -1;
        if (item >= 0) {
            if (pkt[8] >= '0' && pkt[8] <= (rear_select ? '1' : '3') &&
                xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
                (data ? s_data_menu : s_ssb_menu)[item] = pkt[8];
                (data ? s_data_menu_us : s_ssb_menu_us)[item] = now_pkt_us;
                s_last_answer_us = now_pkt_us;
                xSemaphoreGive(s_lock);
            }
            return;
        }
    }

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

    if ((s_rig->has_by_tx && strlen(pkt) == 5 && strncmp(pkt, "RI0", 3) == 0 && pkt[4] == ';') ||
        (s_rig->has_ri && strlen(pkt) == RI_LEN && strncmp(pkt, "RI0", 3) == 0 && pkt[RI_LEN - 1] == ';')) {
        if ((pkt[3] == '0' || pkt[3] == '1') && xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_swr_status_us = now_pkt_us;
            s_last_answer_us = now_pkt_us;
            if (pkt[3] == '1') {
                s_hi_swr_us = now_pkt_us;
            } else {
                s_normal_swr_us = now_pkt_us;
            }
            xSemaphoreGive(s_lock);
        }
    }
    bool ri_busy = false, ri_tx = false;
    if (s_rig->has_ri && parse_ri(pkt, &ri_busy, &ri_tx)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.busy = ri_busy;
            if (s_state.tx != ri_tx) record_tx_transition();
            s_state.tx = ri_tx;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            s_last_tx_status_us = s_last_answer_us;
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

    if (pkt[0] == 'V' && s_rig->can_control && strncmp(pkt, "VS", 2) == 0 && strlen(pkt) == 4 &&
        is_side(pkt[2]) && xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_state.rx_vfo_known = true;
        s_state.rx_vfo_sub = pkt[2] == '1';
        s_last_answer_us = now_pkt_us;
        xSemaphoreGive(s_lock);
        return;
    }
    if (pkt[0] == 'S' && s_rig->can_control && strncmp(pkt, "SH", 2) == 0 && strlen(pkt) == 7 &&
        is_rx_side(s_rig, pkt[2]) && pkt[3] == '0' && all_digits(pkt + 4, 2) &&
        digits_value(pkt + 4, 2) <= 23 &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_rig->dual_rx || s_state.rx_vfo_known) {
            const bool sub = s_rig->dual_rx ? pkt[2] == '1' : s_state.rx_vfo_sub;
            s_state.width_code[sub] = (int8_t)digits_value(pkt + 4, 2);
        }
        s_last_answer_us = now_pkt_us;
        xSemaphoreGive(s_lock);
        return;
    }

    // Both manuals define FT0; as MAIN and FT1; as SUB in the read response.
    if (s_rig->can_tx && strncmp(pkt, "FT", 2) == 0 && strlen(pkt) == 4 && pkt[3] == ';' &&
        (pkt[2] == '0' || pkt[2] == '1')) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.tx_vfo = pkt[2] == '0' ? CAT_TX_VFO_MAIN : CAT_TX_VFO_SUB;
            s_tx_vfo_status_us = now_pkt_us;
            s_last_answer_us = now_pkt_us;
            xSemaphoreGive(s_lock);
        }
        return;
    }

    bool tx_on = false;
    if (s_rig->can_tx && parse_tx(pkt, &tx_on)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (s_state.tx != tx_on) record_tx_transition();
            s_state.tx = tx_on;
            s_state.connected = true;
            s_last_answer_us = esp_timer_get_time();
            s_last_tx_status_us = s_last_answer_us;
            s_last_ptt_status_us = s_last_answer_us;
            s_ptt_mode = pkt[2];
            xSemaphoreGive(s_lock);
        }
        return;
    }

    // Both manuals use RM + item + three digits + reserved 000 + semicolon.
    if (s_rig->can_tx && strlen(pkt) == 10 && strncmp(pkt, "RM", 2) == 0 &&
        pkt[2] >= '4' && pkt[2] <= '6' && all_digits(pkt + 3, 3) &&
        memcmp(pkt + 6, "000;", 4) == 0) {
        const unsigned raw = digits_value(pkt + 3, 3);
        if (raw <= 255 && xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_tx_meters.rig = s_rig->rig;
            s_last_answer_us = now_pkt_us;
            if (pkt[2] == '4') {
                s_tx_meters.alc = raw;
                s_tx_meters.alc_us = now_pkt_us;
            } else if (pkt[2] == '5') {
                s_tx_meters.po = raw;
                s_tx_meters.po_us = now_pkt_us;
            } else {
                s_rm6_reply_us = now_pkt_us;
                if (s_tx_meters.po_us > s_tx_meters.swr_po_us &&
                    now_pkt_us - s_tx_meters.po_us <= 250000) {
                    // RM5 precedes RM6 in each poll. A later SWR cannot refresh an old PO/SWR pair.
                    s_tx_meters.swr = raw;
                    s_tx_meters.swr_us = now_pkt_us;
                    s_tx_meters.swr_po = s_tx_meters.po;
                    s_tx_meters.swr_po_us = s_tx_meters.po_us;
                }
            }
            xSemaphoreGive(s_lock);
        }
        return;
    }

    int sm_main = 0, sm_sub = 0;
    if (parse_rm(pkt, &sm_main, &sm_sub)) {
        // Meter ballistics: the S meter is polled every 100 ms but the page reads it every 400 ms. The FTDX10
        // reports the instantaneous value, so each page read caught a random peak or gap and the needle
        // jumped. Rise at once, fall by SM_RELEASE of the gap per reading (about 0.35 s), like an analogue
        // meter; the page then always sees the recent peak level.
        static float sm_f[2];
        const float v[2] = {(float)sm_main, sm_sub >= 0 ? (float)sm_sub : 0.0f};
        for (int i = 0; i < 2; i++) {
            sm_f[i] = (v[i] >= sm_f[i]) ? v[i] : sm_f[i] + (v[i] - sm_f[i]) * SM_RELEASE;
        }
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.s_main = (uint8_t)(sm_f[0] + 0.5f);
            s_state.s_sub = (uint8_t)(sm_f[1] + 0.5f);
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

static void read_poll(cdc_acm_dev_hdl_t dev, const char *commands, unsigned wait_ms, bool keyed)
{
    for (const char *p = commands; p && *p && !s_disconnected;) {
        const char *end = strchr(p, ';');
        if (!end || end - p >= CAT_CMD_MAX_BYTES) return;
        char cmd[CAT_CMD_MAX_BYTES + 1];
        size_t length = end - p + 1;
        memcpy(cmd, p, length);
        cmd[length] = '\0';
        if (!poll_rejected(cmd)) query(dev, cmd, NULL, 0, wait_ms, keyed);
        p = end + 1;
    }
}

static void send_poll(cdc_acm_dev_hdl_t dev, const char *commands)
{
    read_poll(dev, commands, NORMAL_ANSWER_MS, false);
}

static bool cat_tx_requested(void);

static void initialize_ptt_port(cdc_acm_dev_hdl_t dev)
{
    xSemaphoreTake(s_ptt_lock, portMAX_DELAY);
    if (s_ptt_port == dev && !s_ptt_ready && !s_disconnected) {
        s_ptt_ready = cat_tx(dev, "TX0;") == ESP_OK;
    }
    xSemaphoreGive(s_ptt_lock);
}

static void report_queued_write_failure(const char *cmd, esp_err_t err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_state.write_error_seq;
    snprintf(s_state.write_error, sizeof(s_state.write_error), "%s: %s", cmd, esp_err_to_name(err));
    xSemaphoreGive(s_lock);
}

typedef struct {
    int64_t sent_at, alc_at;
    bool status;
    unsigned meters_until_status;
} keyed_poll_t;

static bool keyed_poll_answered(const rig_profile_t *rig, const keyed_poll_t *poll)
{
    const int64_t sent_at = poll->sent_at;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool answered = poll->status
        ? s_last_ptt_status_us >= sent_at && s_swr_status_us >= sent_at &&
          s_tx_vfo_status_us >= sent_at && s_mode_status_us[0] >= sent_at &&
          (!rig->dual_rx || s_mode_status_us[1] >= sent_at)
        : s_tx_meters.po_us >= sent_at && s_rm6_reply_us >= s_tx_meters.po_us;
    xSemaphoreGive(s_lock);
    return answered;
}

static void poll_ft8_alc(cdc_acm_dev_hdl_t dev, const rig_profile_t *rig, keyed_poll_t *poll)
{
    if (!atomic_load(&s_ft8_alc) || poll->status ||
        esp_timer_get_time() - poll->alc_at < ALC_PERIOD_US ||
        poll_rejected("RM4;") || !keyed_poll_answered(rig, poll)) return;

    // An optional ALC read gets a short budget, only after fresh PO/SWR replies.
    poll->alc_at = esp_timer_get_time();
    query(dev, "RM4;", NULL, 0, ALC_QUERY_MS, true);
}

static void poll_keyed(cdc_acm_dev_hdl_t dev, const rig_profile_t *rig, keyed_poll_t *poll)
{
    const int64_t now = esp_timer_get_time();
    bool recheck_swr = false;
    if (poll->sent_at) {
        if (atomic_load(&s_effective_rejects) != s_keyed_reject_baseline) s_split_keyed_polls = true;
        const int64_t elapsed = now - poll->sent_at;
        // On the FTDX10 a missing reply must leave time for another meter poll
        // before the SWR watchdog reaches its 1.1 s freshness limit.
        const int64_t reply_timeout = rig->rig == CAT_RIG_FTDX10
            ? FTDX10_KEYED_REPLY_TIMEOUT_US : FTX1_KEYED_REPLY_TIMEOUT_US;
        if (elapsed < POLL_PERIOD_MS * 1000 ||
            (elapsed < reply_timeout && !keyed_poll_answered(rig, poll))) {
            return;
        }
        if (rig->rig == CAT_RIG_FTDX10 && poll->status) {
            // Check a possible startup spike again without waiting for two meter polls.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            recheck_swr = s_hi_swr_us >= poll->sent_at && s_normal_swr_us < s_hi_swr_us;
            xSemaphoreGive(s_lock);
        }
        poll_ft8_alc(dev, rig, poll);
    }

    // Record before writing: a reply can arrive before the CDC write returns.
    poll->sent_at = esp_timer_get_time();
    poll->status = recheck_swr || poll->meters_until_status == 0;
    const char *cmds = "RM5;RM6;";
    if (poll->status) {
        cmds = rig->dual_rx ? "RI0;TX;FT;MD0;MD1;" : "TX;RI0;FT;MD0;";
        poll->meters_until_status = rig->rig == CAT_RIG_FTX1 ? 1 : 2;
    } else {
        --poll->meters_until_status;
    }
    s_keyed_reject_baseline = atomic_load(&s_effective_rejects);
    if (s_split_keyed_polls) {
        read_poll(dev, cmds, NORMAL_ANSWER_MS, true);
        poll_ft8_alc(dev, rig, poll);
        // Individual reads already awaited their replies.
        poll->sent_at = 0;
    } else {
        cat_tx_kind(dev, cmds, CAT_LOG_POLL, true);
    }
}

static bool rigctl_write_confirmed(cdc_acm_dev_hdl_t dev, const char *cmd,
                                    const char *readback, const char *expected)
{
    esp_err_t err = cat_tx(dev, cmd);
    if (err == ESP_OK) {
        char answer[RX_BUF_BYTES];
        if (query(dev, readback, answer, sizeof(answer), NORMAL_ANSWER_MS, false) == QUERY_ANSWER &&
            strcmp(answer, expected) == 0) return true;
        err = ESP_ERR_INVALID_RESPONSE;
    }
    report_queued_write_failure(cmd, err);
    return false;
}

static bool run_rigctl_write(cdc_acm_dev_hdl_t dev, cat_cmd_item_t item)
{
    // NET rigctl may enqueue one mode+width transaction. Keep each USB write separate.
    char *second = strchr(item.cmd, ';') + 1;
    char width[8];
    strlcpy(width, second, sizeof(width));
    *second = '\0';
    const bool selecting = strncmp(item.cmd, "VS", 2) == 0;
    const bool split = strncmp(item.cmd, "FT", 2) == 0;
    cat_state_t state;
    cat_get_state(&state);
    const bool active_matches = state.rx_vfo_known && state.rx_vfo_sub == item.sub;
    if (esp_timer_get_time() >= item.expires_us || state.rig != item.rig || !cat_radio_rx_confirmed() ||
        cat_tx_requested() || (item.require_active && !active_matches)) {
        report_queued_write_failure(item.cmd, ESP_ERR_INVALID_STATE);
        return false;
    }
    // An explicit split request supplies the RX side as well as the TX side.
    // Confirm the receiver before issuing FT. Frequency/mode writes never select a VFO.
    if (split && !active_matches) {
        const char *select = item.sub ? "VS1;" : "VS0;";
        if (!rigctl_write_confirmed(dev, select, "VS;", select)) return false;
        cat_get_state(&state);
        if (esp_timer_get_time() >= item.expires_us || !state.rx_vfo_known || state.rx_vfo_sub != item.sub ||
            !cat_radio_rx_confirmed() || cat_tx_requested()) {
            report_queued_write_failure(item.cmd, ESP_ERR_INVALID_STATE);
            return false;
        }
    }

    char readback[6] = {item.cmd[0], item.cmd[1], ';'};
    if (strncmp(item.cmd, "MD", 2) == 0) {
        readback[2] = item.cmd[2];
        readback[3] = ';';
    }
    char expected[16];
    strlcpy(expected, item.cmd, sizeof(expected));
    if (split && !s_rig->dual_rx) expected[2] -= 2;
    if (!rigctl_write_confirmed(dev, item.cmd, readback, expected)) return false;

    if (width[0]) {
        // FTDX10 SH0 and relative VFO requests must still target the same receiver.
        cat_get_state(&state);
        const char mode = item.sub ? state.mode_sub_raw : state.mode_main_raw;
        if (esp_timer_get_time() >= item.expires_us || mode != item.cmd[3] ||
            (item.require_active && (!state.rx_vfo_known || state.rx_vfo_sub != item.sub)) ||
            !cat_radio_rx_confirmed() || cat_tx_requested()) {
            report_queued_write_failure(width, ESP_ERR_INVALID_STATE);
            return false;
        }
        const char *filter = s_rig->dual_rx && item.sub ? "SH1;" : "SH0;";
        return rigctl_write_confirmed(dev, width, filter, width);
    }
    if (selecting) {
        cat_get_state(&state);
        if (!state.rx_vfo_known || state.rx_vfo_sub != item.sub) {
            report_queued_write_failure(item.cmd, ESP_ERR_INVALID_RESPONSE);
            return false;
        }
        send_poll(dev, "FT;");
        send_poll(dev, s_rig->dual_rx && item.sub ? "SH1;" : "SH0;");
    }
    return true;
}

static void poll_loop(cdc_acm_dev_hdl_t dev)
{
    const rig_profile_t *rig = s_rig;
    uint32_t poll_count = 0;
    const bool net_rigctl = rig->can_control && app_config_rigctl_enabled();
    keyed_poll_t keyed_poll = {0};
    bool usb_level_applied = false;   // DATA audio output level already set for the current DATA-USB session

    ESP_LOGI(TAG, "Polling %s", rig->name);

    while (!s_disconnected) {
        observe_usb_events();
        if (s_disconnected || !cat_usb_ready()) break;
        if (rig->can_tx && cat_tx_requested()) {
            poll_keyed(dev, rig, &keyed_poll);
            vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
            continue;
        }
        if (keyed_poll.sent_at) {
            // Refresh radio status immediately after a TX0, before normal polling.
            send_poll(dev, rig->poll_status);
            keyed_poll = (keyed_poll_t){0};
        }
        // 1. Process any pending commands in the queue (already checked against the allow-list when queued)
        cat_cmd_item_t item;
        while (s_cmd_queue && !cat_tx_requested() && xQueueReceive(s_cmd_queue, &item, 0) == pdTRUE) {
            if (item.session != atomic_load(&s_cat_session)) continue;
            if (item.ticket) {
                const unsigned result = run_rigctl_write(dev, item) ? 1u : 2u;
                atomic_store(&s_rigctl_results[item.ticket & 1u], (item.ticket << 2) | result);
                // Leave room for normal CAT polling even with a busy network client.
                break;
            }
            ESP_LOGI(TAG, "Sending CAT command: %s", item.cmd);
            const esp_err_t err = cat_tx(dev, item.cmd);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "CAT write failed (%s): %s", item.cmd, esp_err_to_name(err));
                report_queued_write_failure(item.cmd, err);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            if (strncmp(item.cmd, "MD", 2) == 0 && strlen(item.cmd) == 5) {
                const char readback[] = {'M', 'D', item.cmd[2], ';', '\0'};
                send_poll(dev, readback);
            }
            // SV (swap) / AB / BA (copy) change both VFOs: re-read them so the UI does not
            // show stale values until the next periodic sync
            if (strcmp(item.cmd, "SV;") == 0 || strcmp(item.cmd, "AB;") == 0 || strcmp(item.cmd, "BA;") == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                send_poll(dev, rig->resync_vfo);
            }
        }

        if (rig->can_tx && cat_tx_requested()) {
            continue;
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
            if (rig->can_tx) {
                initialize_ptt_port(dev);
            }
            send_poll(dev, rig->poll_status);
            if (rig->can_tx && app_config_tx_enabled()) {
                send_poll(dev, rig->dual_rx ? "FT;MD0;MD1;" : "FT;MD0;");
            } else if (net_rigctl) {
                send_poll(dev, "FT;");
            }
        }

        // 4. Periodic slow refresh / sync every 5 seconds (50 iterations)
        if ((poll_count % 50) == 0) {
            const int64_t sync_start = esp_timer_get_time();
            send_poll(dev, rig->sync_all);
            if (poll_count == 0) {
                ESP_LOGI(TAG, "%s sync_all: %lld ms", rig->name,
                         (long long)((esp_timer_get_time() - sync_start) / 1000));
            }
            if (net_rigctl) send_poll(dev, rig->dual_rx ? "VS;SH0;SH1;" : "VS;SH0;");
            if (rig->can_tx && app_config_tx_enabled()) {
                if (rig->rig == CAT_RIG_FTDX10) {
                    send_poll(dev, "EX010113;EX010114;EX010415;EX010416;");
                } else if (rig->rig == CAT_RIG_FTX1) {
                    send_poll(dev, "EX010113;EX010413;");
                }
            }
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

static void initialize_reads(cdc_acm_dev_hdl_t dev, const rig_profile_t *rig)
{
    const int64_t started = esp_timer_get_time();
    read_poll(dev, "FA;FB;MD0;MD1;", ANSWER_TIMEOUT_MS, false);
    bool enable_ai = false;
    if (rig->can_control && !s_disconnected && !poll_rejected("AI;")) {
        char answer[RX_BUF_BYTES];
        const query_result_t result = query(dev, "AI;", answer, sizeof(answer), ANSWER_TIMEOUT_MS, false);
        enable_ai = result == QUERY_TIMEOUT || (result == QUERY_ANSWER && strcmp(answer, "AI0;") == 0);
    }
    if (rig->can_control) {
        read_poll(dev, rig->dual_rx ? "AG0;AG1;SQ0;SQ1;" : "AG0;SQ0;", ANSWER_TIMEOUT_MS, false);
        read_poll(dev, rig->poll_status, ANSWER_TIMEOUT_MS, false);
        read_poll(dev, rig->poll_fast, ANSWER_TIMEOUT_MS, false);
        read_poll(dev, rig->dual_rx ? "BP00;BP01;BP10;BP11;" : "BP00;BP01;", ANSWER_TIMEOUT_MS, false);
        read_poll(dev, "FT;", ANSWER_TIMEOUT_MS, false);
    }
    ESP_LOGI(TAG, "%s initial CAT reads: %lld ms", rig->name,
             (long long)((esp_timer_get_time() - started) / 1000));
    if (enable_ai && !s_disconnected && cat_tx(dev, "AI1;") == ESP_OK) {
        // AI may immediately emit readbacks; let them arrive before the next command.
        vTaskDelay(pdMS_TO_TICKS(NORMAL_ANSWER_MS));
    }
}

static void read_cached_baud(void)
{
    nvs_handle_t handle;
    if (nvs_open(CAT_CACHE_NVS, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, CAT_CACHE_BAUD, &s_last_baud);
        nvs_close(handle);
    }
}

static void cache_baud(uint32_t baud)
{
    if (s_last_baud == baud) return;
    s_last_baud = baud;
    nvs_handle_t handle;
    if (nvs_open(CAT_CACHE_NVS, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_u32(handle, CAT_CACHE_BAUD, baud) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
}

static size_t baud_order(uint32_t order[5])
{
    size_t used = 0;
    for (size_t i = 0; i < sizeof(s_default_bauds) / sizeof(s_default_bauds[0]); ++i) {
        if (s_default_bauds[i] == s_last_baud) order[used++] = s_last_baud;
    }
    for (size_t i = 0; i < sizeof(s_default_bauds) / sizeof(s_default_bauds[0]); ++i) {
        if (s_default_bauds[i] != s_last_baud) order[used++] = s_default_bauds[i];
    }
    return used;
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

static void close_cat_port(cdc_acm_dev_hdl_t *dev)
{
    // Invalidate delayed callbacks before the handle can be freed or reused.
    atomic_store(&s_link_starting, true);
    atomic_store(&s_cat_session, 0);
    const unsigned addr = atomic_exchange(&s_cat_addr, 0);
    if (*dev) {
        const esp_err_t err = cdc_acm_host_close(*dev);
        if (err != ESP_OK) ESP_LOGW(TAG, "CDC close at USB address %u: %s", addr, esp_err_to_name(err));
        *dev = NULL;
    }
    atomic_store(&s_rx_restart, true);
    s_rejected_count = 0;
    s_split_keyed_polls = false;
}

static bool is_cp210x(uint8_t addr)
{
    usb_device_handle_t dev;
    esp_err_t err = usb_host_device_open(s_usb_observer, addr, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Skip USB address %u: descriptor open failed: %s (0x%x)",
                 addr, esp_err_to_name(err), (unsigned)err);
        return false;
    }
    const usb_device_desc_t *desc;
    const esp_err_t read_err = usb_host_get_device_descriptor(dev, &desc);
    const unsigned vid = read_err == ESP_OK ? desc->idVendor : 0;
    const unsigned pid = read_err == ESP_OK ? desc->idProduct : 0;
    err = usb_host_device_close(s_usb_observer, dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Skip USB address %u: descriptor close failed: %s (0x%x)",
                 addr, esp_err_to_name(err), (unsigned)err);
        return false;
    }
    if (read_err != ESP_OK) {
        ESP_LOGW(TAG, "Skip USB address %u: descriptor read failed: %s (0x%x)",
                 addr, esp_err_to_name(read_err), (unsigned)read_err);
        return false;
    }
    if (vid != SILABS_VID) {
        ESP_LOGW(TAG, "Skip USB address %u: non-CP210x (VID 0x%04x, PID 0x%04x)", addr, vid, pid);
        return false;
    }
    return true;
}

/** Open the CP210x at USB address addr and return true if it answers ID; as a CAT port */
static bool try_port(uint8_t addr, cdc_acm_dev_hdl_t *dev, uint8_t *opened, bool *no_id, bool cached_only)
{
    *no_id = false;
    observe_usb_events();
    if (!cat_usb_address_ready(addr)) {
        ESP_LOGW(TAG, "Skip USB address %u: 3 s CAT settling period or USB reset still in progress", addr);
        return false;
    }
    if (++s_next_cat_session == 0) ++s_next_cat_session;
    atomic_store(&s_cat_session, s_next_cat_session);
    atomic_store(&s_cat_addr, addr);
    atomic_store(&s_rx_restart, true);
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
        .user_arg = (void *)(uintptr_t)s_next_cat_session,
    };
    esp_err_t err = cdc_acm_host_open(&cfg, dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CDC open at USB address %u, interface %u: %s (0x%x)",
                 addr, cfg.interface_idx, esp_err_to_name(err), (unsigned)err);
        diag("open addr %u: %s", addr, esp_err_to_name(err));
        close_cat_port(dev);
        return false;
    }
    (*opened)++;
    // The CP210x UARTs here are single-interface devices (the CDC driver opened interface 0)
    const uint8_t intf = cfg.interface_idx;
    uint32_t bauds[5];
    const size_t supported = baud_order(bauds);
    const size_t count = cached_only ? 1 : supported;
    size_t attempted = 0;
    for (size_t b = 0; b < count; b++) {
        observe_usb_events();
        if (s_disconnected || !cat_usb_ready()) {
            ESP_LOGW(TAG, "Stop probing USB address %u: %s", addr, s_disconnected ? "disconnected" :
                     "3 s CAT settling period or USB reset still in progress");
            break;
        }
        err = cp210x_setup(*dev, intf, bauds[b]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CDC setup at USB address %u, %lu bps: %s",
                     addr, (unsigned long)bauds[b], esp_err_to_name(err));
            diag("setup addr %u: %s", addr, esp_err_to_name(err));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CAT_BAUD_SETTLE_MS));
        observe_usb_events();
        if (cat_tx_kind(*dev, ";", CAT_LOG_POLL, false) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(CAT_DELIMITER_MS));
        observe_usb_events();
        if (s_disconnected || !cat_usb_ready()) break;

        char answer[RX_BUF_BYTES];
        atomic_store(&s_rx_restart, true);
        s_rx_bytes = 0;
        ++attempted;
        if (query(*dev, "ID;", answer, sizeof(answer), ANSWER_TIMEOUT_MS, false) == QUERY_ANSWER) {
            memcpy(s_rig_id, answer + 2, 4);
            s_rig_id[4] = '\0';
            s_rig = profile_for_id(s_rig_id);
            cache_baud(bauds[b]);
            ESP_LOGI(TAG, "CAT port at USB address %u, interface %u, %lu bps: %s -> %s", addr, intf,
                     (unsigned long)bauds[b], answer, s_rig->name);
            return true;
        }
        ESP_LOGW(TAG, "addr %u @%lu: no ID; answer (%lu bytes received)", addr, (unsigned long)bauds[b],
                 (unsigned long)s_rx_bytes);
        diag("addr %u: no answer, rx %lu B [%s]", addr, (unsigned long)s_rx_bytes, s_rx_tail);
    }
    *no_id = attempted == count && !s_disconnected && cat_usb_ready();
    close_cat_port(dev);
    return false;
}

static bool cat_cached_only(int64_t now)
{
    for (size_t i = 0; i < sizeof(s_default_bauds) / sizeof(*s_default_bauds); ++i) {
        if (s_default_bauds[i] == s_last_baud) {
            return !s_cat_missing_since || now - s_cat_missing_since < CAT_CACHED_ONLY_US;
        }
    }
    return false;
}

static void recover_missing_cat(int64_t now)
{
    if (!s_cat_missing_since || now - s_cat_missing_since < CAT_RECOVER_US ||
        s_cat_recoveries >= CAT_RECOVER_MAX) return;

    if (usb_audio_recover_cat(s_cat_recoveries + 1, CAT_RECOVER_MAX)) {
        s_cat_missing_since = 0;
        ++s_cat_recoveries;
        if (s_cat_recoveries == CAT_RECOVER_MAX) {
            ESP_LOGW(TAG, "CAT USB recovery limit reached; continuing to probe without cycling power");
        }
    }
}

static cdc_acm_dev_hdl_t find_cat_port(void)
{
    observe_usb_events();
    uint8_t addrs[10];
    int count = 0;
    const esp_err_t err = usb_host_device_addr_list_fill(sizeof(addrs), addrs, &count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB address scan failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return NULL;
    }
    bool present[128] = {0};
    for (int i = 0; i < count; ++i) {
        if (addrs[i] < 128) present[addrs[i]] = true;
    }
    taskENTER_CRITICAL(&s_usb_mux);
    for (unsigned addr = 1; addr < 128; ++addr) {
        if (present[addr] && !s_usb_seen_us[addr]) s_usb_seen_us[addr] = esp_timer_get_time() + CAT_SETTLE_US;
        else if (!present[addr]) s_usb_seen_us[addr] = 0;
    }
    taskEXIT_CRITICAL(&s_usb_mux);
    uint8_t opened = 0;
    diag_counts((uint8_t)count, 0, 0);
    if (count == 0) {
        diag("no USB device on the bus");
    }
    int start = 0;
    for (int i = 0; i < count; ++i) {
        if (addrs[i] == s_scan_after_addr) {
            start = (i + 1) % count;
            break;
        }
    }
    // Audio recovery also resets the CAT settling timer.
    taskENTER_CRITICAL(&s_usb_mux);
    const int64_t reset_until = s_usb_reset_until;
    taskEXIT_CRITICAL(&s_usb_mux);
    if (s_cat_missing_since < reset_until) s_cat_missing_since = 0;
    const bool cached_only = cat_cached_only(esp_timer_get_time());
    bool cp210x_present = false;
    bool cp210x_ready = false;
    uint8_t first_unanswered = 0;
    for (int i = 0; i < count; ++i) {
        const uint8_t addr = addrs[(start + i) % count];
        if (!addr || addr >= 128) {
            ESP_LOGW(TAG, "Skip invalid USB address %u", addr);
            continue;
        }
        if (!is_cp210x(addr)) continue;
        cp210x_present = true;
        if (cat_usb_address_ready(addr)) {
            cp210x_ready = true;
            if (!s_cat_missing_since) s_cat_missing_since = esp_timer_get_time();
        }
        cdc_acm_dev_hdl_t dev = NULL;
        bool no_id;
        s_disconnected = false;
        if (try_port(addr, &dev, &opened, &no_id, cached_only)) {
            s_cat_missing_since = 0;
            s_cat_recoveries = 0;
            s_scan_after_addr = 0;
            diag_counts((uint8_t)count, opened, 1);
            diag("%s", "");
            return dev;
        }
        if (no_id && !first_unanswered) first_unanswered = addr;
        diag_counts((uint8_t)count, opened, 0);
    }
    s_scan_after_addr = first_unanswered;
    if (cp210x_ready) recover_missing_cat(esp_timer_get_time());
    if (!cp210x_present) s_cat_missing_since = 0;
    return NULL;
}

static void cat_task(void *arg)
{
    s_task = xTaskGetCurrentTaskHandle();
    read_cached_baud();
    for (;;) {
        s_disconnected = false;
        s_rig = &s_profile_none;          // only discovery probes may be sent before identification
        memset(s_rig_id, 0, sizeof(s_rig_id));
        atomic_store(&s_link_starting, true);
        const int64_t scan_start = esp_timer_get_time();
        cdc_acm_dev_hdl_t dev = find_cat_port();
        if (dev) {
            // Keep CAT controls and PTT unavailable during the initial read-only sequence.
            initialize_reads(dev, s_rig);
            if (s_disconnected || !cat_usb_ready()) {
                close_cat_port(&dev);
                publish(false, 0, 0, 0, 0, 0, 0, "", 0, "");
                continue;
            }
            publish_profile();
            publish(true, -1, -1, -1, -1, -1, 0, NULL, 0, NULL);
            xSemaphoreTake(s_ptt_lock, portMAX_DELAY);
            s_ptt_port = dev;
            s_ptt_ready = false;
            xSemaphoreGive(s_ptt_lock);
            if (s_rig->can_tx) {
                initialize_ptt_port(dev);
            }
            atomic_store(&s_link_starting, false);
            poll_loop(dev);
            ESP_LOGW(TAG, "CAT port lost");
            xSemaphoreTake(s_ptt_lock, portMAX_DELAY);
            s_ptt_port = NULL;
            s_ptt_ready = false;
            close_cat_port(&dev);
            xSemaphoreGive(s_ptt_lock);
            publish(false, 0, 0, 0, 0, 0, 0, "", 0, "");
            s_rig = &s_profile_none;
            memset(s_rig_id, 0, sizeof(s_rig_id));
            publish_profile();
        }
        const bool cached_only = cat_cached_only(esp_timer_get_time());
        const int64_t wait_ms = cached_only
            ? CAT_CACHED_SCAN_MS - (esp_timer_get_time() - scan_start) / 1000 : SCAN_PERIOD_MS;
        vTaskDelay(pdMS_TO_TICKS(wait_ms > 1 ? wait_ms : 1));
    }
}

esp_err_t cat_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ptt_lock = xSemaphoreCreateMutex();
    s_wire_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock && s_ptt_lock && s_wire_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    s_cmd_queue = xQueueCreate(10, sizeof(cat_cmd_item_t));
    ESP_RETURN_ON_FALSE(s_cmd_queue, ESP_ERR_NO_MEM, TAG, "no queue memory");
    const usb_host_client_config_t observer = {
        .max_num_event_msg = 16,
        .flags.notify_dev_removed = true,
        .async.client_event_callback = cat_usb_event,
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&observer, &s_usb_observer), TAG, "CAT USB observer");
    const cdc_acm_host_driver_config_t cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 0,
    };
    ESP_RETURN_ON_ERROR(cdc_acm_host_install(&cfg), TAG, "cdc_acm install");
    BaseType_t ok = xTaskCreatePinnedToCore(cat_task, "cat", 4096, NULL, CAT_TASK_PRIORITY, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "cat task");
    return ESP_OK;
}

// Called with s_lock. A stale or missing readback cannot authorize voice.
static cat_voice_source_t voice_source(void)
{
    const bool rear_select = s_rig->rig == CAT_RIG_FTDX10;
    const int64_t now = esp_timer_get_time();
    if (!s_ssb_menu_us[0] || now - s_ssb_menu_us[0] > MOD_MENU_MAX_AGE_US ||
        (rear_select && (!s_ssb_menu_us[1] || now - s_ssb_menu_us[1] > MOD_MENU_MAX_AGE_US))) {
        return CAT_VOICE_SOURCE_UNKNOWN;
    }
    // FTX-1 AUTO selects USB modulation when the transmitter is keyed by CAT.
    if (s_rig->rig == CAT_RIG_FTX1 && s_ssb_menu[0] == '3') {
        return CAT_VOICE_SOURCE_USB;
    }
    return s_ssb_menu[0] == '1' && (!rear_select || s_ssb_menu[1] == '1')
        ? CAT_VOICE_SOURCE_USB : CAT_VOICE_SOURCE_OTHER;
}

const char *cat_voice_mode_error(bool starting)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char mode = s_reported_mode[s_state.tx_vfo == CAT_TX_VFO_SUB];
    const cat_voice_source_t source = starting ? voice_source() : CAT_VOICE_SOURCE_USB;
    xSemaphoreGive(s_lock);
    if (mode != '1' && mode != '2') return "Select USB or LSB for microphone TX";
    if (source == CAT_VOICE_SOURCE_UNKNOWN) return "Waiting for SSB modulation source";
    if (source != CAT_VOICE_SOURCE_USB) {
        return s_rig->rig == CAT_RIG_FTX1 ? "Set SSB MOD SOURCE to USB or AUTO"
                                         : "Set SSB modulation source to USB";
    }
    return NULL;
}

const char *cat_data_mode_error(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool rear_select = s_rig->rig == CAT_RIG_FTDX10;
    const int64_t now = esp_timer_get_time();
    const char *reason = NULL;
    if (!s_data_menu_us[0] || now - s_data_menu_us[0] > MOD_MENU_MAX_AGE_US ||
        (rear_select && (!s_data_menu_us[1] || now - s_data_menu_us[1] > MOD_MENU_MAX_AGE_US))) {
        reason = "Waiting for DATA-USB modulation source";
    } else if (s_data_menu[0] != '1' || (rear_select && s_data_menu[1] != '1')) {
        reason = rear_select ? "Set DATA MOD SOURCE to REAR and REAR SELECT to USB"
                             : "Set MODE DATA MOD SOURCE to USB";
    }
    xSemaphoreGive(s_lock);
    return reason;
}

void cat_get_state(cat_state_t *state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *state = s_state;
    if (atomic_load(&s_link_starting) || s_disconnected) {
        state->connected = false;
        state->can_control = false;
    }
    state->voice_source = voice_source();
    const int64_t now = esp_timer_get_time();
    state->tx_radio_known = s_state.connected && !s_disconnected && s_last_tx_status_us > 0 &&
                            now - s_last_tx_status_us <= RADIO_STATUS_MAX_AGE_US;
    state->tx_radio = state->tx_radio_known && s_state.tx;
    if (!s_state.connected || s_disconnected || s_tx_vfo_status_us == 0 ||
        esp_timer_get_time() - s_tx_vfo_status_us > RADIO_STATUS_MAX_AGE_US) {
        state->tx_vfo = CAT_TX_VFO_UNKNOWN;
    }
    state->age_ms = s_state.connected ? (uint32_t)((esp_timer_get_time() - s_last_answer_us) / 1000) : 0;
    xSemaphoreGive(s_lock);
}

cat_tx_meters_t cat_get_tx_meters(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const cat_tx_meters_t meters = s_tx_meters;
    xSemaphoreGive(s_lock);
    return meters;
}

cat_write_stats_t cat_get_write_stats(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cat_write_stats_t stats = s_write_stats;
    xSemaphoreGive(s_lock);
    stats.budget_ms = TX_TIMEOUT_MS;
    stats.keyed_poll_budget_ms = keyed_write_timeout_ms();
    return stats;
}

bool cat_radio_rx_after(int64_t after_us)
{
    if (!s_lock || atomic_load(&s_link_starting)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool confirmed = s_state.connected && !s_disconnected && !s_state.tx && s_ptt_mode == '0' &&
                           s_last_ptt_status_us > after_us &&
                           esp_timer_get_time() - s_last_ptt_status_us <= RADIO_STATUS_MAX_AGE_US;
    xSemaphoreGive(s_lock);
    return confirmed;
}

bool cat_radio_rx_confirmed(void)
{
    if (!s_lock || atomic_load(&s_link_starting)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool confirmed = s_state.connected && !s_disconnected && !s_state.tx && s_last_tx_status_us > 0 &&
                           esp_timer_get_time() - s_last_tx_status_us <= RADIO_STATUS_MAX_AGE_US;
    xSemaphoreGive(s_lock);
    return confirmed;
}

bool cat_radio_tx(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t max_age = s_state.tx_cat ? KEYED_CAT_MAX_AGE_US : RADIO_STATUS_MAX_AGE_US;
    const bool active = s_state.connected && !s_disconnected && s_state.tx &&
                        esp_timer_get_time() - s_last_tx_status_us <= max_age;
    xSemaphoreGive(s_lock);
    return active;
}

static bool cat_tx_requested(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool requested = s_state.tx_cat;
    xSemaphoreGive(s_lock);
    return requested;
}

void cat_enable_ft8_alc(void)
{
    atomic_store(&s_ft8_alc, true);
}

esp_err_t cat_ptt(bool on)
{
    if (!s_ptt_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!on) {
        atomic_store(&s_ft8_alc, false);
        // Stop submitting telemetry while TX0 is waiting for the CDC OUT endpoint.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_unkey_requested = true;
        xSemaphoreGive(s_lock);
    }
    if (xSemaphoreTake(s_ptt_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = s_ptt_port && !s_disconnected && (!on || (s_ptt_ready && !atomic_load(&s_link_starting)))
                          ? cat_tx(s_ptt_port, on ? "TX1;" : "TX0;") : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_state.tx_cat != on) record_tx_transition();
        s_state.tx_cat = on;
        s_unkey_requested = false;
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_ptt_lock);
    return err;
}

const char *cat_ptt_fault(int64_t started, int64_t grace_us)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const char *reason = NULL;
    const int64_t startup_end = started + FTDX10_HI_SWR_STARTUP_US;
    // An early FTDX10 flag needs a fresh normal report within the startup window.
    // A missing normal reply is a fault, and a flag after startup always latches.
    const bool hi_swr = s_hi_swr_us && s_hi_swr_us >= started &&
        (s_rig->rig != CAT_RIG_FTDX10 || s_hi_swr_us >= startup_end ||
         (now >= startup_end && !(s_normal_swr_us > s_hi_swr_us && s_normal_swr_us < startup_end)));
    if (hi_swr) {
        reason = "High SWR";
    } else if (s_disconnected || !s_state.connected) {
        reason = "CAT disconnected";
    } else if (now - started >= KEYED_CAT_MAX_AGE_US &&
               now - s_last_answer_us >= KEYED_CAT_MAX_AGE_US) {
        reason = "CAT lost";
    } else if (now - started >= grace_us && s_last_ptt_status_us >= started && s_ptt_mode != '1') {
        reason = "Radio TX status unexpected";
    } else if (now - started >= KEYED_CAT_MAX_AGE_US &&
               (now - s_last_ptt_status_us >= KEYED_CAT_MAX_AGE_US ||
                now - s_swr_status_us >= KEYED_CAT_MAX_AGE_US)) {
        reason = "Radio TX status unavailable";
    }
    xSemaphoreGive(s_lock);
    return reason;
}

/** Only sideband modes are allowed while remote PTT has no audio. */
static bool ptt_sideband_mode(char mode)
{
    return mode == '1' || mode == '2' || mode == '8' || mode == 'C';
}

const char *cat_ptt_mode_error(int64_t keyed_at)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const char *reason = NULL;
    const int64_t oldest = keyed_at && now - keyed_at < KEYED_CAT_MAX_AGE_US
        ? keyed_at - RADIO_STATUS_MAX_AGE_US
        : now - (keyed_at ? KEYED_CAT_MAX_AGE_US : RADIO_STATUS_MAX_AGE_US);
    if (!s_mode_status_us[0] || s_mode_status_us[0] < oldest ||
        s_state.mode_main_raw != s_reported_mode[0]) {
        reason = "Waiting for MAIN mode confirmation";
    } else if (!ptt_sideband_mode(s_reported_mode[0])) {
        reason = "Set MAIN mode to USB, LSB, DATA-USB or DATA-LSB";
    } else if (s_state.tx_vfo == CAT_TX_VFO_UNKNOWN || !s_tx_vfo_status_us ||
               s_tx_vfo_status_us < oldest) {
        reason = "Waiting for TX VFO confirmation";
    } else if (s_state.tx_vfo == CAT_TX_VFO_SUB) {
        if (!s_rig->dual_rx) {
            reason = "Select MAIN for remote PTT";
        } else if (!s_mode_status_us[1] || s_mode_status_us[1] < oldest ||
            s_state.mode_sub_raw != s_reported_mode[1]) {
            reason = "Waiting for SUB mode confirmation";
        } else if (!ptt_sideband_mode(s_reported_mode[1])) {
            reason = "Set SUB mode to USB, LSB, DATA-USB or DATA-LSB";
        }
    }
    xSemaphoreGive(s_lock);
    return reason;
}

/** Queue a typed, allow-listed write. NET rigctl can add a second command for bandwidth. */
static esp_err_t cat_enqueue_item(const char *cmd, unsigned ticket, bool sub, bool require_active)
{
    if (!cat_cmds_allowed(cmd, ticket ? 2 : 1)) {
        ESP_LOGE(TAG, "Refused a CAT command that is not on the allow-list");
        return ESP_ERR_INVALID_ARG;
    }
    const unsigned session = atomic_load(&s_cat_session);
    if (!s_cmd_queue || atomic_load(&s_link_starting) || s_disconnected || cat_tx_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ticket && !cat_radio_rx_confirmed()) return ESP_ERR_INVALID_STATE;
    cat_cmd_item_t item = {.ticket = ticket, .session = session,
                           .expires_us = esp_timer_get_time() + 1200000, .sub = sub,
                           .require_active = require_active, .rig = s_rig->rig};
    strlcpy(item.cmd, cmd, sizeof(item.cmd));

    if (xQueueSend(s_cmd_queue, &item, ticket ? 0 : pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

static esp_err_t cat_enqueue(const char *cmd)
{
    return cat_enqueue_item(cmd, 0, false, false);
}

esp_err_t cat_rigctl_set_freq(bool sub, uint32_t hz, bool require_active, unsigned ticket)
{
    char cmd[16];
    if (hz < s_rig->freq_min || hz > s_rig->freq_max) return ESP_ERR_INVALID_ARG;
    snprintf(cmd, sizeof(cmd), "F%c%09lu;", sub ? 'B' : 'A', (unsigned long)hz);
    return cat_enqueue_item(cmd, ticket, sub, require_active);
}

esp_err_t cat_rigctl_set_mode(bool sub, char mode, int width_hz, bool require_active, unsigned ticket)
{
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "MD%c%c;", sub ? '1' : '0', mode);
    const unsigned fixed = rigctl_width_hz(s_rig->rig, mode, -1);
    if (fixed > 0) {
        if (width_hz > 0 && (unsigned)width_hz != fixed) return ESP_ERR_NOT_SUPPORTED;
    } else if (width_hz > 0) {
        const int code = rigctl_width_code(s_rig->rig, mode, width_hz);
        if (code < 0 || code > 23) return ESP_ERR_NOT_SUPPORTED;
        // On FTDX10 SH0 addresses the sole receiver, whichever VFO is active.
        require_active |= !s_rig->dual_rx;
        snprintf(cmd + 5, sizeof(cmd) - 5, "SH%c0%02u;", s_rig->dual_rx && sub ? '1' : '0',
                 (unsigned)code);
    }
    return cat_enqueue_item(cmd, ticket, sub, require_active);
}

esp_err_t cat_rigctl_select_vfo(bool sub, unsigned ticket)
{
    return cat_enqueue_item(sub ? "VS1;" : "VS0;", ticket, sub, false);
}

esp_err_t cat_rigctl_set_split(bool rx_sub, bool tx_sub, bool require_active, unsigned ticket)
{
    const char *cmd = s_rig->dual_rx ? (tx_sub ? "FT1;" : "FT0;") : (tx_sub ? "FT3;" : "FT2;");
    return cat_enqueue_item(cmd, ticket, rx_sub, require_active);
}

int cat_rigctl_result(unsigned ticket)
{
    const unsigned result = atomic_load(&s_rigctl_results[ticket & 1u]);
    return (result >> 2) == ticket ? ((result & 3u) == 1u ? 1 : -1) : 0;
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
