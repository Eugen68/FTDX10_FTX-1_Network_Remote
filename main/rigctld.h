/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#include <stdbool.h>

typedef struct {
    bool enabled;
    bool listening;
    unsigned port;
    unsigned clients;
    unsigned commands;
    unsigned rejected;
} rigctld_stats_t;

void rigctld_start(void);
rigctld_stats_t rigctld_stats(void);
