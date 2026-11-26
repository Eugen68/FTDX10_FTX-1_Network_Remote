/*
 * Firmware version and build date / time (see build_info.c).
 */

#pragma once

#define NETWORK_REMOTE_NAME "FTDX10 / FTX-1 Network Remote"

// Firmware version, shown in the GUI header and returned by /status
#define FTX1_VERSION "1.9.41-yo9va"

// Build date and time of this firmware, e.g. "Oct 30 2026 21:56:13" (build machine local time)
const char *build_info_get(void);
