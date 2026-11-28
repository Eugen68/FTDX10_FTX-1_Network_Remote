/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_PREFS_JSON_BYTES 1024
#define UI_PREFS_FIELD_COUNT 27

typedef struct {
    uint32_t present;
    char values[UI_PREFS_FIELD_COUNT][20];
} ui_prefs_t;

bool ui_prefs_parse(const char *json, ui_prefs_t *patch, bool *reset);
void ui_prefs_merge(ui_prefs_t *settings, const ui_prefs_t *patch);
int ui_prefs_format(const ui_prefs_t *settings, char *out, size_t capacity);
