/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * Yaesu CAT over TCP for OmniRig and other programs that expect the radio on a serial port.
 * On the PC, a virtual COM port mapped to TCP (for example HW Virtual Serial Port) connects
 * the program to this server. See yaesu_cat_protocol.h for what is answered and forwarded.
 */

#include <stdbool.h>
#include "yaesu_cat_protocol.h"

#define YAESU_CATD_DEFAULT_PORT 4533

typedef struct {
    bool enabled;
    bool listening;
    bool debug;
    unsigned port;
    unsigned clients;
    unsigned commands;
    unsigned rejected;
    unsigned writes;
    unsigned opened, closed, accept_errors, send_errors;
    char last_rejected[YAESU_CAT_CMD_MAX + 1];
} yaesu_catd_stats_t;

void yaesu_catd_start(void);
yaesu_catd_stats_t yaesu_catd_stats(void);
