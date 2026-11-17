/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "app_config.h"
#include "console_menu.h"
#include <fcntl.h>
#include <unistd.h>

#define UART_PORT_NUM UART_NUM_0
#define BOOT_BUTTON_GPIO GPIO_NUM_0

static bool s_io_initialized = false;
static bool s_uart_driver_installed = false;

static void init_console_io(void)
{
    if (s_io_initialized) return;

    // Configure BOOT button (GPIO 0, active LOW with internal pull-up)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);

    // Set non-blocking mode on stdin
    int flags = fcntl(fileno(stdin), F_GETFL, 0);
    fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);

    if (!uart_is_driver_installed(UART_PORT_NUM)) {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_param_config(UART_PORT_NUM, &uart_config) == ESP_OK) {
            uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
            if (uart_driver_install(UART_PORT_NUM, 256, 256, 0, NULL, 0) == ESP_OK) {
                s_uart_driver_installed = true;
            }
        }
    } else {
        s_uart_driver_installed = true;
    }

    s_io_initialized = true;
}

static void console_write_str(const char *str)
{
    init_console_io();
    fputs(str, stdout);
    fflush(stdout);
}

/** Read a single character with timeout in ms from UART0, stdin, or ROM. Returns -1 on timeout. */
static int read_char_timeout(int timeout_ms)
{
    init_console_io();
    int waited = 0;
    while (waited < timeout_ms) {
        // Try UART0 driver first
        if (s_uart_driver_installed) {
            uint8_t uc = 0;
            int len = uart_read_bytes(UART_PORT_NUM, &uc, 1, 0);
            if (len > 0) {
                return (int)uc;
            }
        }

        // Try standard VFS stdin
        char sc = 0;
        ssize_t n = read(fileno(stdin), &sc, 1);
        if (n > 0) {
            return (unsigned char)sc;
        }

        // Try ROM Serial output RX
        uint8_t rc = 0;
        if (esp_rom_output_rx_one_char(&rc) == 0) {
            return (int)rc;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return -1;
}

/** Read a single line with backspace handling and optional masking */
static void read_line(char *buf, size_t max_len, bool mask)
{
    size_t pos = 0;
    buf[0] = '\0';

    while (1) {
        int c = read_char_timeout(100);
        if (c < 0) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            console_write_str("\r\n");
            buf[pos] = '\0';
            break;
        } else if (c == '\b' || c == 0x7F) { // Backspace or DEL
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                console_write_str("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (pos + 1 < max_len) {
                buf[pos++] = (char)c;
                buf[pos] = '\0';
                char echo[2] = {mask ? '*' : (char)c, '\0'};
                console_write_str(echo);
            }
        }
    }
}

static void mask_string(const char *src, char *dst, size_t dst_len)
{
    size_t len = strlen(src);
    if (len == 0) {
        strlcpy(dst, "(empty / disabled)", dst_len);
        return;
    }
    if (len <= 4) {
        strlcpy(dst, "****", dst_len);
        return;
    }
    snprintf(dst, dst_len, "%c", src[0]);
    for (size_t i = 1; i < len - 1 && strlen(dst) + 2 < dst_len; i++) {
        strlcat(dst, "*", dst_len);
    }
    char last[2] = {src[len - 1], '\0'};
    strlcat(dst, last, dst_len);
}

/** Accept a Basic-auth user ID: 1..max_len printable ASCII characters, no ':' (it separates user and password) */
static bool user_id_is_valid(const char *s, size_t max_len)
{
    size_t len = strlen(s);
    if (len == 0 || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] < 33 || s[i] > 126 || s[i] == ':') {
            return false;
        }
    }
    return true;
}

/** Ask for a secret that protects network access (OTA / web UI). Returns true when cfg value was changed. */
static bool prompt_secret(const char *prompt, char *out, size_t out_size, const char *cleared_msg, const char *updated_msg)
{
    console_write_str(prompt);
    char temp[APP_CONFIG_PASS_MAX_LEN + 1];
    read_line(temp, sizeof(temp), true);
    if (temp[0] == '\0') {
        return false;
    }
    if (strcmp(temp, "CLEAR") == 0 || strcmp(temp, "none") == 0) {
        out[0] = '\0';
        console_write_str(cleared_msg);
        return true;
    }
    if (strlen(temp) < APP_CONFIG_MIN_SECRET_LEN) {
        char err[96];
        snprintf(err, sizeof(err), "-> Rejected: the password must have at least %d characters\r\n", APP_CONFIG_MIN_SECRET_LEN);
        console_write_str(err);
        return false;
    }
    strlcpy(out, temp, out_size);
    console_write_str(updated_msg);
    return true;
}

static void print_config_summary(const app_config_t *cfg)
{
    char masked_wifi_pass[64];
    char masked_ota_pass[64];
    char masked_web_pass[64];
    mask_string(cfg->wifi_pass, masked_wifi_pass, sizeof(masked_wifi_pass));
    mask_string(cfg->ota_pass, masked_ota_pass, sizeof(masked_ota_pass));
    mask_string(cfg->web_pass, masked_web_pass, sizeof(masked_web_pass));

    char msg[1024];
    snprintf(msg, sizeof(msg),
        "\r\n"
        "==================================================\r\n"
        "           CURRENT RUNTIME CONFIGURATION          \r\n"
        "==================================================\r\n"
        " 1) Wi-Fi SSID       : %s\r\n"
        " 2) Wi-Fi Password   : %s\r\n"
        " 3) OTA User ID      : %s\r\n"
        " 4) OTA Password     : %s\r\n"
        " 9) Web User ID      : %s\r\n"
        " 0) Web Password     : %s\r\n"
        " h) Host name        : %s.local\r\n"
        "==================================================\r\n",
        cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(not set)",
        masked_wifi_pass,
        cfg->ota_user[0] ? cfg->ota_user : "ota",
        masked_ota_pass,
        cfg->web_user[0] ? cfg->web_user : "admin",
        cfg->web_pass[0] ? masked_web_pass : (cfg->ota_pass[0] ? "(empty: uses the OTA password)" : "(empty: web UI refuses all requests)"),
        cfg->hostname
    );
    console_write_str(msg);
}

void console_menu_run(void)
{
    init_console_io();
    app_config_t cfg;
    app_config_get(&cfg);

    console_write_str(
        "\r\n"
        "**************************************************\r\n"
        "*       FTX-1 Companion Configuration Menu       *\r\n"
        "**************************************************\r\n"
    );

    bool running = true;
    while (running) {
        print_config_summary(&cfg);
        console_write_str(
            "Options:\r\n"
            "  1: Set Wi-Fi SSID\r\n"
            "  2: Set Wi-Fi Password\r\n"
            "  3: Set OTA User ID\r\n"
            "  4: Set OTA Password (12+ chars)\r\n"
            "  5: Show passwords in plain text\r\n"
            "  6: Save to NVS and Restart device\r\n"
            "  7: Exit and continue boot without saving\r\n"
            "  8: Erase NVS configuration (Reset to defaults)\r\n"
            "  9: Set Web User ID (web UI, audio stream, /cat)\r\n"
            "  0: Set Web Password (12+ chars)\r\n"
            "  h: Set Host name (mDNS / DHCP)\r\n"
            "--------------------------------------------------\r\n"
            "Enter selection (0-9, h): "
        );

        char opt[8] = {0};
        read_line(opt, sizeof(opt), false);

        switch (opt[0]) {
            case '1': {
                char prompt[128];
                snprintf(prompt, sizeof(prompt), "Enter Wi-Fi SSID [Current: %s] (Enter to keep): ", cfg.wifi_ssid);
                console_write_str(prompt);
                char temp[APP_CONFIG_SSID_MAX_LEN + 1];
                read_line(temp, sizeof(temp), false);
                if (strlen(temp) > 0) {
                    strlcpy(cfg.wifi_ssid, temp, sizeof(cfg.wifi_ssid));
                    char res[128];
                    snprintf(res, sizeof(res), "-> Wi-Fi SSID set to: %s\r\n", cfg.wifi_ssid);
                    console_write_str(res);
                }
                break;
            }
            case '2': {
                console_write_str("Enter Wi-Fi Password (leave empty for open, Enter to keep): ");
                char temp[APP_CONFIG_PASS_MAX_LEN + 1];
                read_line(temp, sizeof(temp), true);
                if (strlen(temp) > 0) {
                    if (strcmp(temp, "CLEAR") == 0 || strcmp(temp, "none") == 0) {
                        cfg.wifi_pass[0] = '\0';
                        console_write_str("-> Wi-Fi Password cleared (Open network)\r\n");
                    } else {
                        strlcpy(cfg.wifi_pass, temp, sizeof(cfg.wifi_pass));
                        console_write_str("-> Wi-Fi Password updated\r\n");
                    }
                }
                break;
            }
            case '3': {
                char prompt[128];
                snprintf(prompt, sizeof(prompt), "Enter OTA User ID [Current: %s] (Enter to keep): ", cfg.ota_user);
                console_write_str(prompt);
                char temp[APP_CONFIG_OTA_USER_MAX_LEN + 1];
                read_line(temp, sizeof(temp), false);
                if (strlen(temp) > 0) {
                    if (user_id_is_valid(temp, APP_CONFIG_OTA_USER_MAX_LEN)) {
                        strlcpy(cfg.ota_user, temp, sizeof(cfg.ota_user));
                        char res[128];
                        snprintf(res, sizeof(res), "-> OTA User ID set to: %s\r\n", cfg.ota_user);
                        console_write_str(res);
                    } else {
                        console_write_str("-> Rejected: use printable characters without ':' or spaces\r\n");
                    }
                }
                break;
            }
            case '4': {
                prompt_secret("Enter OTA Password, 12+ chars (CLEAR to disable OTA, Enter to keep): ",
                              cfg.ota_pass, sizeof(cfg.ota_pass),
                              "-> OTA Password cleared (OTA disabled)\r\n",
                              "-> OTA Password updated\r\n");
                break;
            }
            case '9': {
                char prompt[128];
                snprintf(prompt, sizeof(prompt), "Enter Web User ID [Current: %s] (Enter to keep): ", cfg.web_user);
                console_write_str(prompt);
                char temp[APP_CONFIG_WEB_USER_MAX_LEN + 1];
                read_line(temp, sizeof(temp), false);
                if (strlen(temp) > 0) {
                    if (user_id_is_valid(temp, APP_CONFIG_WEB_USER_MAX_LEN)) {
                        strlcpy(cfg.web_user, temp, sizeof(cfg.web_user));
                        char res[128];
                        snprintf(res, sizeof(res), "-> Web User ID set to: %s\r\n", cfg.web_user);
                        console_write_str(res);
                    } else {
                        console_write_str("-> Rejected: use printable characters without ':' or spaces\r\n");
                    }
                }
                break;
            }
            case '0': {
                prompt_secret("Enter Web Password, 12+ chars (CLEAR to fall back to the OTA password, Enter to keep): ",
                              cfg.web_pass, sizeof(cfg.web_pass),
                              "-> Web Password cleared (the OTA password is used, or the web UI is locked)\r\n",
                              "-> Web Password updated\r\n");
                break;
            }
            case 'h':
            case 'H': {
                char prompt[256];
                snprintf(prompt, sizeof(prompt),
                         "Enter Host name without \".local\" [Current: %s] (a-z, 0-9, '-'; DEFAULT to reset; Enter to keep): ",
                         cfg.hostname);
                console_write_str(prompt);
                char temp[APP_CONFIG_HOSTNAME_MAX_LEN + 1];
                read_line(temp, sizeof(temp), false);
                if (strlen(temp) > 0) {
                    if (strcmp(temp, "DEFAULT") == 0) {
                        strlcpy(cfg.hostname, APP_CONFIG_DEFAULT_HOSTNAME, sizeof(cfg.hostname));
                    } else if (app_config_hostname_is_valid(temp)) {
                        strlcpy(cfg.hostname, temp, sizeof(cfg.hostname));
                    } else {
                        console_write_str("-> Rejected: use lowercase a-z, 0-9 and '-' (not at the start or end), max 63 characters\r\n");
                        break;
                    }
                    char res[128];
                    snprintf(res, sizeof(res), "-> Host name set to: %s.local\r\n", cfg.hostname);
                    console_write_str(res);
                }
                break;
            }
            case '5': {
                char msg[768];
                snprintf(msg, sizeof(msg),
                    "\r\n--- PLAIN TEXT CREDENTIALS ---\r\n"
                    "Wi-Fi SSID    : %s\r\n"
                    "Wi-Fi Password: %s\r\n"
                    "OTA User ID   : %s\r\n"
                    "OTA Password  : %s\r\n"
                    "Web User ID   : %s\r\n"
                    "Web Password  : %s\r\n"
                    "Host name     : %s.local\r\n"
                    "------------------------------\r\n"
                    "Press Enter to return to menu...",
                    cfg.wifi_ssid[0] ? cfg.wifi_ssid : "(empty)",
                    cfg.wifi_pass[0] ? cfg.wifi_pass : "(empty / open)",
                    cfg.ota_user[0] ? cfg.ota_user : "(empty)",
                    cfg.ota_pass[0] ? cfg.ota_pass : "(empty / disabled)",
                    cfg.web_user[0] ? cfg.web_user : "(empty)",
                    cfg.web_pass[0] ? cfg.web_pass : "(empty / OTA password or locked)",
                    cfg.hostname
                );
                console_write_str(msg);
                char dummy[4];
                read_line(dummy, sizeof(dummy), false);
                break;
            }
            case '6': {
                console_write_str("Saving configuration to NVS...\r\n");
                esp_err_t err = app_config_save(&cfg);
                if (err == ESP_OK) {
                    console_write_str("[OK] Configuration saved successfully. Rebooting in 1s...\r\n");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                } else {
                    char err_msg[128];
                    snprintf(err_msg, sizeof(err_msg), "[ERROR] Failed to save configuration: %s\r\n", esp_err_to_name(err));
                    console_write_str(err_msg);
                }
                break;
            }
            case '7': {
                console_write_str("Exiting configuration menu without saving.\r\n");
                running = false;
                break;
            }
            case '8': {
                console_write_str("Are you sure you want to erase NVS config? (type 'yes' to confirm): ");
                char confirm[8];
                read_line(confirm, sizeof(confirm), false);
                if (strcmp(confirm, "yes") == 0 || strcmp(confirm, "YES") == 0) {
                    app_config_erase();
                    app_config_load(&cfg);
                    console_write_str("[OK] NVS configuration erased to defaults.\r\n");
                } else {
                    console_write_str("Aborted.\r\n");
                }
                break;
            }
            default:
                console_write_str("Invalid selection. Please enter 0-9 or h.\r\n");
                break;
        }
    }
}

bool console_menu_check_and_run(int timeout_seconds)
{
    init_console_io();

    // Give a brief moment for USB JTAG / UART host connection to settle
    vTaskDelay(pdMS_TO_TICKS(300));

    app_config_t cfg;
    app_config_get(&cfg);

    // If BOOT button (GPIO 0) is held during power-on/reset, enter menu immediately
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        console_write_str("\r\n[BOOT Button Pressed] Entering configuration menu...\r\n");
        console_menu_run();
        app_config_get(&cfg);
        return app_config_is_valid(&cfg);
    }

    if (!app_config_is_valid(&cfg)) {
        console_write_str("\r\n[!] Wi-Fi SSID is not configured in NVS. Entering configuration menu automatically...\r\n");
        console_menu_run();
        app_config_get(&cfg);
        return app_config_is_valid(&cfg);
    }

    if (timeout_seconds <= 0) {
        return true;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
        "\r\n=================================================================\r\n"
        " FTX-1 Companion Booting (SSID: \"%s\")\r\n"
        " Press [BOOT button] or [Enter] within %d seconds for Config Menu...\r\n"
        "=================================================================\r\n",
        cfg.wifi_ssid, timeout_seconds
    );
    console_write_str(msg);

    int total_steps = timeout_seconds * 10;
    bool trigger_menu = false;

    for (int i = 0; i < total_steps; i++) {
        // Check BOOT button (active LOW)
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            trigger_menu = true;
            break;
        }

        int c = read_char_timeout(100);
        if (c >= 0) {
            if (c == 'c' || c == 'C' || c == 'm' || c == 'M' || c == '\r' || c == '\n' || c == ' ') {
                trigger_menu = true;
                break;
            }
        }
        if ((i + 1) % 10 == 0) {
            int remaining = timeout_seconds - ((i + 1) / 10);
            if (remaining > 0) {
                char rem_msg[16];
                snprintf(rem_msg, sizeof(rem_msg), "... %d\r\n", remaining);
                console_write_str(rem_msg);
            }
        }
    }

    if (trigger_menu) {
        console_write_str("\r\nEntering configuration menu...\r\n");
        console_menu_run();
    } else {
        console_write_str("Booting with saved configuration...\r\n\r\n");
    }

    app_config_get(&cfg);
    return app_config_is_valid(&cfg);
}
