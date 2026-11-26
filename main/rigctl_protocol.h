/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include "cat.h"

#include "rigctld.h"
#define RIGCTL_REPLY_SIZE 640

typedef enum { RIGCTL_FREQ, RIGCTL_MODE, RIGCTL_VFO, RIGCTL_SPLIT } rigctl_action_t;

typedef struct {
    rigctl_action_t action;
    bool sub;
    uint32_t value;
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
