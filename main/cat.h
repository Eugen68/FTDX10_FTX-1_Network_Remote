/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Interface to the CAT control code (cat.c): a snapshot of the radio's state (frequency, mode, S meter,
 * AF gain, squelch, notch, ...) and the functions that change it.
 *
 * Supported radios (detected from the answer to "ID;"): Yaesu FTX-1 (ID 0840) and Yaesu FTDX10 (ID 0761).
 * The FTX-1 has two receivers (MAIN / SUB). The FTDX10 has one receiver: "sub" then means VFO-B
 * (frequency and mode only); AF gain, squelch, notch and the S meter exist for MAIN only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Radio model, detected from the answer to "ID;" */
typedef enum {
    CAT_RIG_NONE = 0,       /*!< No CAT port open yet */
    CAT_RIG_UNKNOWN,        /*!< A port answered ID; with a model this firmware does not know: read-only */
    CAT_RIG_FTX1,           /*!< Yaesu FTX-1 (ID 0840) */
    CAT_RIG_FTDX10,         /*!< Yaesu FTDX10 (ID 0761) */
} cat_rig_t;

/**
 * @brief Radio state read over CAT (the FTX-1's CP210x USB serial port)
 */
typedef enum {
    CAT_TX_VFO_UNKNOWN,
    CAT_TX_VFO_MAIN,
    CAT_TX_VFO_SUB,
} cat_tx_vfo_t;

typedef enum { CAT_VOICE_SOURCE_UNKNOWN, CAT_VOICE_SOURCE_OTHER, CAT_VOICE_SOURCE_USB } cat_voice_source_t;

typedef struct {
    cat_voice_source_t voice_source;
    bool connected;         /*!< A CAT port is open and answering */
    cat_rig_t rig;          /*!< Detected radio model */
    char rig_name[12];      /*!< "FTX-1", "FTDX10", "UNKNOWN" or "" */
    char rig_id[5];         /*!< The 4 characters of the ID; answer, "" before a radio answered */
    bool dual_rx;           /*!< The radio has a separate SUB receiver (AF gain / squelch / notch / S meter) */
    uint16_t sq_max;        /*!< Highest squelch value the radio accepts (255 FTX-1, 100 FTDX10) */
    uint32_t freq_min;      /*!< Lowest frequency accepted by cat_set_freq(), Hz */
    uint32_t freq_max;      /*!< Highest frequency accepted by cat_set_freq(), Hz */
    bool can_control;       /*!< Set commands are allowed (false for an unknown radio) */
    bool c4fm;              /*!< The radio has the C4FM modes */
    bool busy;              /*!< Squelch is open on the receiver reported by RI (BUSY) */
    cat_tx_vfo_t tx_vfo;    /*!< Transmitter selected by the radio, from FT; */
    bool rx_vfo_known;
    bool rx_vfo_sub;
    int8_t width_code[2];     /*!< SH code for each receiver, -1 if unavailable */
    int8_t key_pitch;         /*!< CW key pitch, KP code 0-75 (300-1050 Hz in 10 Hz steps), -1 if unread */
    uint8_t pa_bands;         /*!< PA P1 values: 1 (FTDX10), 3 (FTX-1: HF/50, VHF, UHF), 0 = no front-end controls */
    uint8_t att_max;          /*!< Highest attenuator value: 3 (FTDX10: 6/12/18 dB), 1 (FTX-1: ON) */
    int8_t preamp[3];         /*!< PA per P1: 0 IPO, 1 AMP1, 2 AMP2 (HF/50); VHF/UHF 0 OFF, 1 ON; -1 unread */
    int8_t att;               /*!< RA: 0 OFF, 1.. as above; -1 unread */
    int16_t rf_gain[2];       /*!< RG 0-255 per receiver, -1 unread */
    int8_t agc[2];            /*!< GT per receiver: 0 OFF, 1 FAST, 2 MID, 3 SLOW, 4-6 AUTO-FAST/MID/SLOW; -1 unread */
    bool nb_switch;           /*!< NB / DNR have an ON/OFF command (FTDX10); else level 0 is OFF (FTX-1) */
    uint8_t dnr_max;          /*!< Highest DNR level (15 FTDX10, 10 FTX-1), 0 = no noise controls */
    int8_t nb_on[2], nb_level[2];   /*!< Noise blanker per receiver, level 0-10; -1 unread */
    int8_t dnr_on[2], dnr_level[2]; /*!< DNR per receiver; -1 unread */
    int8_t dnf[2];            /*!< Auto notch (DNF) per receiver; -1 unread */
    int16_t if_shift[2];      /*!< IF shift in Hz, -1200..+1200 */
    bool if_shift_known[2];
    int16_t scope_level_x10;  /*!< FTDX10 scope LEVEL in tenths of a dB (-300 .. 300) */
    bool scope_level_known;
    int16_t tx_power_w;       /*!< TX power setting (PC), watts; -1 if unread */
    uint8_t tx_power_head;    /*!< FTX-1 PC P1: 1 field head (5-10 W), 2 SPA-1 (5-100 W); 0 = FTDX10 or unread */
    bool tx_cat;
    bool tx_radio;
    bool tx_radio_known;
    bool tx;                /*!< The radio is transmitting (RI P4 = 1) */
    uint8_t s_main;         /*!< S meter, MAIN side, 0-255 */
    uint8_t s_sub;          /*!< S meter, SUB side, 0-255 */
    uint32_t freq_main;     /*!< Frequency in Hz (MAIN/VFO-A) */
    uint32_t freq_sub;      /*!< Frequency in Hz (SUB/VFO-B) */
    char mode_main[16];     /*!< Mode name, e.g. "USB", "LSB", "FM", "AM" (MAIN) */
    char mode_sub[16];      /*!< Mode name, e.g. "USB", "LSB", "FM", "AM" (SUB) */
    char mode_main_raw;     /*!< Raw mode character ('1'-'9', 'A'-'F') */
    char mode_sub_raw;      /*!< Raw mode character ('1'-'9', 'A'-'F') */
    uint8_t vol_main;       /*!< AF Gain / Volume, MAIN side, 0-255 */
    uint8_t vol_sub;        /*!< AF Gain / Volume, SUB side, 0-255 */
    uint8_t sq_main;        /*!< Hardware Squelch level, MAIN side, 0-255 */
    uint8_t sq_sub;         /*!< Hardware Squelch level, SUB side, 0-255 */
    bool notch_main;        /*!< Manual notch ON, MAIN side */
    bool notch_sub;         /*!< Manual notch ON, SUB side */
    uint16_t notch_hz_main; /*!< Manual notch frequency in Hz (10-3200), MAIN side; 0 = not read yet */
    uint16_t notch_hz_sub;  /*!< Manual notch frequency in Hz (10-3200), SUB side; 0 = not read yet */
    uint32_t age_ms;        /*!< Time since the last answer */
    uint8_t devices;        /*!< USB devices seen on the last scan (hubs included) */
    uint8_t cp210x;         /*!< Of those, CP210x (VID 0x10C4) devices opened */
    bool waiting_for_cat;   /*!< Enumerated CP210x without a valid ID */
    unsigned cat_usb_resets; /*!< Recovery attempts since the last valid ID */
    uint8_t answered;       /*!< Of those, devices that answered ID; */
    uint32_t write_error_seq;
    char write_error[80];   /*!< Last failed queued CAT write */
    char last_error[80];    /*!< Last scan problem, "" if none */
} cat_state_t;

/** @brief Kind of a CAT log entry */
typedef enum {
    CAT_LOG_CMD = 0,      /*!< A command sent on request (from the page, the menu item, the port search) */
    CAT_LOG_POLL,         /*!< A periodic query or its answer (S meter, BUSY, the 5 s sync) */
    CAT_LOG_ANSWER,       /*!< Anything else the radio sent (answers, auto information) */
    CAT_LOG_BLOCKED,      /*!< A command the allow-list refused: it was NOT sent */
    CAT_LOG_ERROR,        /*!< A command that could not be written to the port */
} cat_log_kind_t;

#define CAT_LOG_TEXT_LEN 24

/** @brief One entry of the CAT traffic log */
typedef struct {
    uint32_t seq;                   /*!< Increases by one per entry, never 0 */
    uint32_t ms;                    /*!< Time since boot, ms */
    char dir;                       /*!< 'T' sent to the radio (or refused), 'R' received from it */
    uint8_t kind;                   /*!< cat_log_kind_t */
    char text[CAT_LOG_TEXT_LEN];    /*!< The command with its ';', printable ASCII only */
} cat_log_entry_t;

/**
 * @brief Copy the log entries newer than since_seq, oldest first
 * @return Number of entries copied (at most max). The log keeps the last 160 entries.
 */
size_t cat_log_read(uint32_t since_seq, cat_log_entry_t *out, size_t max);

/** Copy the latest RM5/RM6 entries through the given uptime, newest first. */
size_t cat_log_recent_tx_meters(uint32_t through_ms, cat_log_entry_t *out, size_t max);

/**
 * @brief Start the CAT client. Requires usb_audio_init() (it installs the USB Host Library) first.
 *
 * A new CP210x settles before receiving CAT bytes. Prefer the last working baud, then probe
 * 115200, 38400, 19200, 9600 and 4800 bps as needed. A valid ID selects the radio profile.
 */
esp_err_t cat_init(void);

/**
 * @brief Copy the latest radio state
 */
void cat_get_state(cat_state_t *state);

/** True only when a recent CAT status report confirmed the radio is not transmitting. */
bool cat_radio_rx_confirmed(void);

/** NET rigctl writes use the CAT allow-list and require freshly confirmed RX. */
esp_err_t cat_rigctl_set_freq(bool sub, uint32_t hz, bool require_active, unsigned ticket);
esp_err_t cat_rigctl_set_mode(bool sub, char mode, int width_hz, bool require_active, unsigned ticket);
esp_err_t cat_rigctl_select_vfo(bool sub, unsigned ticket);
esp_err_t cat_rigctl_set_split(bool rx_sub, bool tx_sub, bool require_active, unsigned ticket);
/** Returns 0 while pending, 1 on success, or -1 on CAT failure. */
int cat_rigctl_result(unsigned ticket);

typedef struct {
    cat_rig_t rig;
    unsigned po, swr, swr_po, alc;
    int64_t po_us, swr_us, swr_po_us, alc_us;
} cat_tx_meters_t;

cat_tx_meters_t cat_get_tx_meters(void);

typedef struct {
    unsigned calls, timeouts, over_budget, max_us;
} cat_write_timing_t;

typedef struct {
    cat_write_timing_t all, keyed, meters;
    unsigned budget_ms, keyed_poll_budget_ms;
} cat_write_stats_t;

cat_write_stats_t cat_get_write_stats(void);
bool cat_radio_rx_after(int64_t after_us);
bool cat_radio_tx(void);
esp_err_t cat_ptt(bool on);
void cat_enable_ft8_alc(void);
const char *cat_ptt_fault(int64_t started, int64_t grace_us);
const char *cat_ptt_mode_error(int64_t keyed_at);
const char *cat_voice_mode_error(bool starting);
const char *cat_data_mode_error(void);

/*
 * The only free-form input is the page's CAT console (cat_send_console), and it goes through the same
 * allow-list, never keys the radio, and sends nothing if a command is refused. Every other command is built
 * here from
 * typed arguments, and cat.c only lets an allow-list of commands (frequency, mode, AF gain, squelch,
 * notch, VFO swap/copy, split, pre-amp, attenuator, RF gain, AGC, NB, DNR, DNF, IF shift, scope span, power) reach the radio. Nothing in this firmware can make the radio transmit.
 */

#define CAT_CONSOLE_MAX_BYTES 64
#define CAT_CONSOLE_MAX_CMDS  8

/**
 * @brief Send a line typed in the page's CAT LOG console, e.g. "FA014074000;" or "FA;MD0;".
 * Every command must be on the allow-list (refused ones are logged as BLOCKED), transmit (TX1 / TX2) is always
 * refused, and if any command is refused nothing is sent. why gets the reason on failure.
 */
esp_err_t cat_send_console(const char *line, char *why, size_t why_size);

/**
 * @brief Set the TX power (CAT "PC"): FTDX10 5-100 W; FTX-1 5-10 W (field head) or 5-100 W (SPA-1), as the radio
 * reports. Only with remote TX enabled, and called only from the authenticated TX panel. It never keys the radio.
 */
esp_err_t cat_set_tx_power(unsigned watts);

/** @brief Swap the MAIN and SUB VFOs (CAT "SV") */
esp_err_t cat_swap_vfo(void);

/** @brief Copy MAIN to SUB (CAT "AB") */
esp_err_t cat_copy_main_to_sub(void);

/** @brief Copy SUB to MAIN (CAT "BA") */
esp_err_t cat_copy_sub_to_main(void);

/** @brief Split: select the transmitter VFO (CAT "FT"). It never keys the radio; refused while TX is requested. */
esp_err_t cat_set_split(bool tx_sub);

/** @brief Pre-amp / IPO (CAT "PA"). band: 0 (FTDX10; FTX-1 HF/50), 1 VHF, 2 UHF (FTX-1). value: 0 IPO,
 *  1 AMP1, 2 AMP2 on HF/50; 0 OFF, 1 ON on VHF/UHF */
esp_err_t cat_set_preamp(uint8_t band, uint8_t value);

/** @brief RF attenuator (CAT "RA"): 0 OFF, FTDX10 1-3 = 6/12/18 dB, FTX-1 1 = ON */
esp_err_t cat_set_att(uint8_t value);

/** @brief RF gain 0-255 (CAT "RG") */
esp_err_t cat_set_rf_gain(bool sub, uint8_t value);

/** @brief AGC (CAT "GT"): 0 OFF, 1 FAST, 2 MID, 3 SLOW, 4 AUTO */
esp_err_t cat_set_agc(bool sub, uint8_t value);

/** Level argument for cat_set_nb / cat_set_dnr: keep the radio's level */
#define CAT_LEVEL_KEEP 0xFF

/** @brief Noise blanker: FTDX10 NB on/off + NL 0-10; FTX-1 NL 1-10 (OFF = 000) */
esp_err_t cat_set_nb(bool sub, bool on, uint8_t level);

/** @brief DNR: FTDX10 NR on/off + RL 1-15; FTX-1 RL 1-10 (OFF = 00) */
esp_err_t cat_set_dnr(bool sub, bool on, uint8_t level);

/** @brief Auto notch / DNF (CAT "BC") */
esp_err_t cat_set_dnf(bool sub, bool on);

/**
 * CW AUTO ZERO IN (ZI0; / ZI1;): the radio tunes the receiver onto the CW signal, as its ZIN key does.
 * Set only, never transmits. FTX-1: MAIN or SUB; FTDX10: one receiver (P1 fixed 0), so sub is refused.
 */
esp_err_t cat_zero_in(bool sub);

/** @brief IF shift (CAT "IS"), -1200..+1200 Hz in 20 Hz steps */
esp_err_t cat_set_if_shift(bool sub, int hz);

/** @brief FTDX10 scope span (CAT "SS05"): 0 1 kHz, 1 2k, 2 5k, 3 10k, 4 20k, 5 50k, 6 100k, 7 200k, 8 500k, 9 1 MHz.
 *  The ACC scope frames report the span the radio uses. */
esp_err_t cat_set_scope_span(uint8_t code);

/** @brief FTDX10 scope LEVEL (CAT "SS04"), in tenths of a dB: -300 .. +300 in steps of 5 (0.5 dB) */
esp_err_t cat_set_scope_level(int tenths);

/**
 * FTDX10 (one receiver): MD0 / MD1 answer for the active VFO and the other one, so mode_main / mode_sub hold
 * the active VFO's mode first. True when VFO-B is active, i.e. mode_main is B's mode and mode_sub is A's.
 */
static inline bool cat_modes_swapped(const cat_state_t *s)
{
    return !s->dual_rx && s->rx_vfo_known && s->rx_vfo_sub;
}

/** @brief Set the mode of VFO-A or VFO-B, whichever is active (the FTDX10's MD0 / MD1 are relative) */
esp_err_t cat_set_vfo_mode(bool vfo_b, const char *mode_name);

/**
 * @brief Set frequency in Hz
 * @param sub true for SUB (VFO-B), false for MAIN (VFO-A)
 * @param freq_hz Frequency in Hz within the radio's range (FTX-1 30 kHz - 470 MHz, FTDX10 30 kHz - 75 MHz);
 *                ESP_ERR_INVALID_ARG outside it
 */
esp_err_t cat_set_freq(bool sub, uint32_t freq_hz);

/**
 * @brief Set operating mode
 * @param sub true for SUB (VFO-B), false for MAIN (VFO-A)
 * @param mode_char CAT mode character ('1'-'9', 'A'-'F')
 */
esp_err_t cat_set_mode_char(bool sub, char mode_char);

/**
 * @brief Set operating mode by name (e.g. "USB", "LSB", "CW", "FM", "AM", "DATA-USB", etc.)
 * @param sub true for SUB (VFO-B), false for MAIN (VFO-A)
 * @param mode_name Mode name
 */
esp_err_t cat_set_mode(bool sub, const char *mode_name);

/**
 * @brief Set AF Gain / Volume (0-255)
 * @param sub true for SUB (VFO-B), false for MAIN (VFO-A)
 * @param volume Volume level (0-255)
 */
esp_err_t cat_set_volume(bool sub, uint8_t volume);

/**
 * @brief Set hardware Squelch level (0-255 on the FTX-1, 0-100 on the FTDX10)
 * @param sub true for SUB (VFO-B), false for MAIN (VFO-A)
 * @param squelch Squelch level (0-255)
 */
esp_err_t cat_set_squelch(bool sub, uint8_t squelch);

/**
 * @brief Manual notch ON/OFF (CAT "BP", P2=0)
 * @param sub true for SUB, false for MAIN
 */
esp_err_t cat_set_notch(bool sub, bool on);

/**
 * @brief Manual notch frequency (CAT "BP", P2=1)
 * @param sub true for SUB, false for MAIN
 * @param hz Frequency in Hz, 10-3200 (the radio takes steps of 10 Hz)
 */
esp_err_t cat_set_notch_freq(bool sub, uint16_t hz);

/**
 * @brief Set Radio Power State (CAT "PS")
 * @param on true for ON (PS1;), false for OFF (PS0;)
 * @note Turning power OFF disconnects USB on most rigs, so waking up over USB CAT is generally not supported.
 */
esp_err_t cat_set_power(bool on);

#ifdef __cplusplus
}
#endif

void cat_usb_reset_begin(void);
void cat_usb_reset_end(void);
unsigned cat_rejected_commands(void);
/** The command the radio last answered "?;" to and how long ago (s); false if none since boot */
bool cat_last_rejected(char *out, size_t size, uint32_t *age_s);
