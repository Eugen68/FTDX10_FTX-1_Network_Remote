/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "ui_prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { NUMBER, STEP, SPAN, TEXT, CALL, GRID, BOOL } field_type_t;
static const struct {
    const char *key;
    field_type_t type;
    int min, max;
    const char *choices;
} fields[] = {
    {.key = "stepMainCW", .type = STEP},
    {.key = "stepMainSSB", .type = STEP},
    {.key = "stepMainDATA", .type = STEP},
    {.key = "stepMainAM", .type = STEP},
    {.key = "stepMainFM", .type = STEP},
    {.key = "stepSubCW", .type = STEP},
    {.key = "stepSubSSB", .type = STEP},
    {.key = "stepSubDATA", .type = STEP},
    {.key = "stepSubAM", .type = STEP},
    {.key = "stepSubFM", .type = STEP},
    {.key = "txTone", .type = TEXT, .choices = "|off|single|dual|voice|"},
    {.key = "txLevel", .type = NUMBER, .min = -60, .max = -6}, {.key = "micGain", .type = NUMBER, .min = -36, .max = 0},
    {.key = "ft8Call", .type = CALL}, {.key = "ft8Grid", .type = GRID},
    {.key = "ft8Hz", .type = NUMBER, .min = 200, .max = 3000}, {.key = "ft8Slot", .type = TEXT, .choices = "|even|odd|"}, {.key = "ft8Level", .type = NUMBER, .min = -60, .max = -6},
    {.key = "dtCorrTenths", .type = NUMBER, .min = -10, .max = 20}, {.key = "decoderMain", .type = BOOL}, {.key = "decoderSub", .type = BOOL},
    {.key = "wfSource", .type = TEXT, .choices = "|auto|audio|acc|"},
    {.key = "spanMain", .type = SPAN}, {.key = "spanSub", .type = SPAN}, {.key = "wfBaseMain", .type = NUMBER, .min = 0, .max = 200}, {.key = "wfBaseSub", .type = NUMBER, .min = 0, .max = 200},
    {.key = "font", .type = TEXT, .choices = "|font-chakra|font-chakra-italic|font-jetbrains|font-rajdhani|font-sharetech|font-system|"},
};
_Static_assert(sizeof(fields) / sizeof(*fields) == UI_PREFS_FIELD_COUNT && UI_PREFS_FIELD_COUNT < 32,
               "preference schema size");

static void skip_space(const char **p)
{
    while (**p && strchr(" \r\n\t", **p)) ++*p;
}

static bool take(const char **p, char next)
{
    skip_space(p);
    if (**p != next) return false;
    ++*p;
    return true;
}

static bool read_string(const char **p, char *out, size_t size)
{
    if (!take(p, '"')) return false;
    size_t n = 0;
    while (**p && **p != '"') {
        const unsigned char c = (unsigned char)**p;
        if (n + 1 >= size || c < 0x20 || c > 0x7e || c == '\\') return false;
        out[n++] = *(*p)++;
    }
    out[n] = '\0';
    if (**p != '"') return false;
    ++*p;
    return true;
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool read_integer(const char **p, long *value)
{
    skip_space(p);
    const char *start = *p;
    const char *digits = *p + (**p == '-');
    const char *end = digits;
    while (digit(*end)) ++end;
    if (end == digits || end - digits > 8 || (*digits == '0' && end - digits > 1)) return false;
    *value = strtol(start, NULL, 10);
    *p = end;
    return true;
}

static bool valid_call(const char *s)
{
    if (strlen(s) > 11) return false;
    for (; *s; ++s) {
        if (!digit(*s) && (*s < 'A' || *s > 'Z') && *s != '/') return false;
    }
    return true;
}

static bool valid_grid(const char *s)
{
    return !*s || (strlen(s) == 4 && s[0] >= 'A' && s[0] <= 'R' &&
                   s[1] >= 'A' && s[1] <= 'R' && digit(s[2]) && digit(s[3]));
}

static bool parse_value(const char **p, unsigned i, char *out, size_t size)
{
    const field_type_t type = fields[i].type;
    if (type == BOOL) {
        skip_space(p);
        const char *value = strncmp(*p, "true", 4) == 0 ? "true" : "false";
        const size_t n = strlen(value);
        if (strncmp(*p, value, n) != 0) return false;
        *p += n;
        memcpy(out, value, n + 1);
        return true;
    }
    if (type == TEXT || type == CALL || type == GRID) {
        if (!read_string(p, out, size)) return false;
        if (type == CALL) return valid_call(out);
        if (type == GRID) return valid_grid(out);
        char choice[24];
        snprintf(choice, sizeof(choice), "|%s|", out);
        return *out && !strchr(out, '|') && strstr(fields[i].choices, choice);
    }

    long value;
    if (!read_integer(p, &value)) return false;
    if (type == NUMBER && (value < fields[i].min || value > fields[i].max)) return false;
    if (type == SPAN && value != 1000 && value != 3500 && value != 6000) return false;
    if (type == STEP) {
        static const long steps[] = {10, 100, 500, 1000, 5000, 10000, 12500, 20000, 25000, 100000, 1000000};
        size_t j = 0;
        while (j < sizeof(steps) / sizeof(*steps) && steps[j] != value) ++j;
        if (j == sizeof(steps) / sizeof(*steps)) return false;
    }
    snprintf(out, size, "%ld", value);
    return true;
}

bool ui_prefs_parse(const char *json, ui_prefs_t *patch, bool *reset)
{
    *patch = (ui_prefs_t){0};
    *reset = false;
    if (strlen(json) >= UI_PREFS_JSON_BYTES || !take(&json, '{')) return false;
    if (!take(&json, '}')) {
        do {
            char key[24];
            if (!read_string(&json, key, sizeof(key)) || !take(&json, ':')) return false;
            if (strcmp(key, "reset") == 0) {
                skip_space(&json);
                if (patch->present || strncmp(json, "true", 4) != 0) return false;
                json += 4;
                if (!take(&json, '}')) return false;
                *reset = true;
                break;
            }
            unsigned i = 0;
            while (i < UI_PREFS_FIELD_COUNT && strcmp(key, fields[i].key) != 0) ++i;
            if (i == UI_PREFS_FIELD_COUNT || (patch->present & (1u << i)) ||
                !parse_value(&json, i, patch->values[i], sizeof(patch->values[i]))) return false;
            patch->present |= 1u << i;
            if (take(&json, '}')) break;
            if (!take(&json, ',')) return false;
        } while (true);
    }
    skip_space(&json);
    return *json == '\0';
}

void ui_prefs_merge(ui_prefs_t *settings, const ui_prefs_t *patch)
{
    for (unsigned i = 0; i < UI_PREFS_FIELD_COUNT; ++i) {
        if (patch->present & (1u << i)) strcpy(settings->values[i], patch->values[i]);
    }
    settings->present |= patch->present;
}

int ui_prefs_format(const ui_prefs_t *settings, char *out, size_t capacity)
{
    if (capacity < 3) return -1;
    out[0] = '{';
    size_t used = 1;
    for (unsigned i = 0; i < UI_PREFS_FIELD_COUNT; ++i) {
        if (!(settings->present & (1u << i))) continue;
        const field_type_t type = fields[i].type;
        const char *quote = type == TEXT || type == CALL || type == GRID ? "\"" : "";
        const int n = snprintf(out + used, capacity - used, "%s\"%s\":%s%s%s",
                               used == 1 ? "" : ",", fields[i].key, quote, settings->values[i], quote);
        if (n < 0 || (size_t)n >= capacity - used - 1) return -1;
        used += (size_t)n;
    }
    out[used++] = '}';
    out[used] = '\0';
    return (int)used;
}
