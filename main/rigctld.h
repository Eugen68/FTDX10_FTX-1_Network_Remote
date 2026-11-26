/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include <stdbool.h>

#define RIGCTL_LINE_SIZE 96

typedef struct {
    bool enabled;
    bool listening;
    bool debug;
    unsigned port;
    unsigned clients;
    unsigned commands;
    unsigned rejected;
    char last_rejected[RIGCTL_LINE_SIZE];
} rigctld_stats_t;

void rigctld_start(void);
rigctld_stats_t rigctld_stats(void);
