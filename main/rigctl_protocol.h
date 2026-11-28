/* SPDX-License-Identifier: MIT */
#pragma once

#include "cat.h"

#include "rigctld.h"
#define RIGCTL_REPLY_SIZE 640

typedef enum { RIGCTL_FREQ, RIGCTL_MODE, RIGCTL_VFO, RIGCTL_SPLIT } rigctl_action_t;

typedef struct {
    rigctl_action_t action;
    bool sub;            // CAT target or, for split, the receive side
    bool require_active; // target was resolved from the active VFO
    uint32_t value;      // frequency, mode code or split transmit side
    int width;
} rigctl_set_t;

typedef bool (*rigctl_set_fn)(const rigctl_set_t *set, void *context);

typedef struct {
    size_t length;
    bool rejected;
    bool close;
} rigctl_reply_t;

rigctl_reply_t rigctl_command(char *line, const cat_state_t *cat, rigctl_set_fn set, void *context,
                             char reply[RIGCTL_REPLY_SIZE]);
const char *rigctl_mode_name(char code);
char rigctl_mode_code(const char *name);
unsigned rigctl_width_hz(cat_rig_t rig, char mode, int code);
int rigctl_width_code(cat_rig_t rig, char mode, int hz);
