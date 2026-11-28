/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * Yaesu CAT emulation for network clients such as OmniRig (through a virtual COM port mapped to TCP).
 *
 * Reads are answered from the radio state the board already keeps, in the exact format of the radio
 * that is connected, so clients that read fixed-length replies stay in step. Writes produce no reply,
 * as on the radio. Rejected writes are dropped silently for the same reason. Nothing here can key the
 * transmitter: TX1 / TX2 are refused.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cat.h"

#define YAESU_CAT_CMD_MAX   32   // longest command accepted, without the ';'
#define YAESU_CAT_REPLY_MAX 48   // longest reply (IF / OI on the FTX-1 is 30 bytes)

typedef enum {
    YAESU_SET_FREQ,        // value: Hz
    YAESU_SET_MODE,        // value: CAT mode character
    YAESU_SELECT_VFO,      // sub: VFO-B / SUB
    YAESU_SET_TX_VFO,      // sub: transmit on VFO-B / SUB
    YAESU_SWAP_VFO,
    YAESU_COPY_A_TO_B,
    YAESU_COPY_B_TO_A,
} yaesu_action_t;

typedef struct {
    yaesu_action_t action;
    bool sub;
    uint32_t value;
} yaesu_set_t;

/** Apply one write. Returns false if the board refused it. */
typedef bool (*yaesu_set_fn)(const yaesu_set_t *set, void *context);

typedef struct {
    bool ai;   // the client asked for auto information (AI1;)
} yaesu_session_t;

typedef struct {
    size_t length;   // bytes in reply (0: nothing to send)
    bool rejected;   // unknown command or refused write
    bool write;      // the command was a write (applied or refused)
} yaesu_reply_t;

/**
 * Handle one command. cmd is the text before ';' (any case). The reply, if any, is written to reply
 * and NUL terminated. No reply is produced while no radio answers CAT, as when the radio is off.
 */
yaesu_reply_t yaesu_cat_command(const char *cmd, const cat_state_t *cat, yaesu_session_t *session,
                                yaesu_set_fn set, void *context, char reply[YAESU_CAT_REPLY_MAX]);

/** IF (sub = false) or OI (sub = true) reply for the connected radio. Returns its length. */
size_t yaesu_cat_info(const cat_state_t *cat, bool sub, char out[YAESU_CAT_REPLY_MAX]);

/** Auto information: replies for the values that changed between old and now (FA, FB, MD0, MD1). */
size_t yaesu_cat_changes(const cat_state_t *old, const cat_state_t *now, char *out, size_t capacity);
