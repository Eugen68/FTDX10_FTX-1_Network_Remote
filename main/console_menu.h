/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Serial setup menu (see console_menu.c): how it is started from main.c.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if interactive menu should be launched (3-second key check or unconfigured NVS).
 *
 * If SSID is not configured, or if the user presses 'c'/'m'/Enter during the 3-second prompt,
 * the interactive UART configuration menu will run until the user chooses to save and restart,
 * or exit.
 *
 * @param timeout_seconds Seconds to wait for keypress if config is valid (e.g. 3).
 * @return true if configuration is ready to proceed to Wi-Fi connection.
 */
bool console_menu_check_and_run(int timeout_seconds);

/**
 * @brief Run the interactive UART configuration menu directly.
 */
void console_menu_run(void);

#ifdef __cplusplus
}
#endif
