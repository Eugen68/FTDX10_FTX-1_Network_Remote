/*
 * Build date and time, embedded in the firmware and shown in the browser header and /status.
 */

#include "build_info.h"

// CMakeLists.txt makes this file depend on every other source and on the embedded HTML,
// so it is recompiled (and the stamp refreshed) on every build that changes the firmware.
const char *build_info_get(void)
{
    return __DATE__ " " __TIME__;
}
