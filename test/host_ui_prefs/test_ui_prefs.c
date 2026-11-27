/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_prefs.h"

static void check_field(const char *key, const char *value, bool valid)
{
    ui_prefs_t patch;
    bool reset;
    char input[128];
    snprintf(input, sizeof(input), "{\"%s\":%s}", key, value);
    assert(ui_prefs_parse(input, &patch, &reset) == valid);
    if (valid) assert(patch.present && !reset);
}

int main(void)
{
    static const struct { const char *key; int min, max; } limits[] = {
        {"txLevel", -60, -6}, {"ft8Level", -60, -6}, {"micGain", -36, 0},
        {"ft8Hz", 200, 3000}, {"dtCorrTenths", -10, 20}, {"wfBaseMain", 0, 200}, {"wfBaseSub", 0, 200},
    };
    for (size_t i = 0; i < sizeof(limits) / sizeof(*limits); ++i) {
        char number[24];
        snprintf(number, sizeof(number), "%d", limits[i].min);
        check_field(limits[i].key, number, true);
        snprintf(number, sizeof(number), "%d", limits[i].max);
        check_field(limits[i].key, number, true);
        snprintf(number, sizeof(number), "%d", limits[i].min - 1);
        check_field(limits[i].key, number, false);
        snprintf(number, sizeof(number), "%d", limits[i].max + 1);
        check_field(limits[i].key, number, false);
    }
    const char *groups[] = {"CW", "SSB", "DATA", "AM", "FM"};
    const char *sides[] = {"Main", "Sub"};
    for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < 5; ++j) {
            char key[20];
            snprintf(key, sizeof(key), "step%s%s", sides[i], groups[j]);
            check_field(key, "10", true);
            check_field(key, "12500", true);
            check_field(key, "1000000", true);
            check_field(key, "9", false);
            check_field(key, "1000001", false);
        }
    }
    static const struct { const char *key, *valid, *invalid; } values[] = {
        {"txTone", "\"voice\"", "\"ft8\""}, {"ft8Call", "\"YO9VA/P\"", "\"ABCDEFGHIJKL\""},
        {"ft8Call", "\"\"", "\"<script>\""}, {"ft8Grid", "\"KN23\"", "\"SN23\""},
        {"ft8Grid", "\"\"", "\"KN23AA\""}, {"ft8Slot", "\"odd\"", "\"any\""},
        {"decoderMain", "true", "1"}, {"decoderSub", "false", "\"false\""},
        {"wfSource", "\"acc\"", "\"auto|audio\""}, {"font", "\"font-chakra-italic\"", "\"font-foo\""},
        {"spanMain", "1000", "100"}, {"spanSub", "6000", "6001"},
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        check_field(values[i].key, values[i].valid, true);
        check_field(values[i].key, values[i].invalid, false);
    }

    ui_prefs_t settings, patch;
    bool reset;
    char result[UI_PREFS_JSON_BYTES];
    assert(ui_prefs_parse(" { \"reset\" : true } ", &patch, &reset) && reset);
    assert(ui_prefs_parse("{ \"stepMainCW\":500,\"wfSource\":\"auto\"}", &settings, &reset) && !reset);
    assert(ui_prefs_parse("{\"stepSubCW\":100,\"wfSource\":\"acc\"}", &patch, &reset) && !reset);
    ui_prefs_merge(&settings, &patch);
    assert(ui_prefs_format(&settings, result, sizeof(result)) > 0);
    assert(strcmp(result, "{\"stepMainCW\":500,\"stepSubCW\":100,\"wfSource\":\"acc\"}") == 0);
    assert(ui_prefs_parse(result, &patch, &reset) && patch.present == settings.present);
    assert(ui_prefs_format(&settings, result, 5) == -1);
    const char *invalid[] = {
        "{", "[]", "{\"wfSource\":\"\"}", "{\"wfSource\":null}", "{\"wfSource\":\"acc\",}",
        "{\"micGain\":-06}", "{\"micGain\":-6.0}", "{\"micGain\":-6e0}",
        "{\"micGain\":99999999999999}", "{\"micGain\":-6,\"micGain\":-7}",
        "{\"ptt\":true}", "{\"ft8Enabled\":true}", "{\"mode\":\"USB\"}",
        "{\"reset\":false}", "{\"reset\":true,\"micGain\":-2}", "{\"micGain\":-2,\"reset\":true}",
        "{\"font\":\"font-\\u0063hakra\"}", "{}{}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        assert(!ui_prefs_parse(invalid[i], &patch, &reset));
    }
    memset(result, ' ', sizeof(result) - 1);
    result[sizeof(result) - 1] = '\0';
    assert(!ui_prefs_parse(result, &patch, &reset));
    puts("UI preference limits, JSON validation, merge and reset passed");
}
