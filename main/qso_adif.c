/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "qso_adif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CALL, GRID, SENT, RCVD, FREQ, ON, OFF, MYCALL, MYGRID, FIELD_COUNT };
static const char *const keys[FIELD_COUNT] = {"call", "grid", "sent", "rcvd", "freq", "on", "off", "mycall", "mygrid"};
#define REQUIRED ((1u << CALL) | (1u << SENT) | (1u << RCVD) | (1u << FREQ) | (1u << ON) | (1u << OFF) | (1u << MYCALL))

static const struct {
    uint32_t low, high;
    const char *name;
} bands[] = {
    {1800000, 2000000, "160m"}, {3500000, 4000000, "80m"}, {5060000, 5450000, "60m"},
    {7000000, 7300000, "40m"}, {10100000, 10150000, "30m"}, {14000000, 14350000, "20m"},
    {18068000, 18168000, "17m"}, {21000000, 21450000, "15m"}, {24890000, 24990000, "12m"},
    {28000000, 29700000, "10m"}, {50000000, 54000000, "6m"}, {70000000, 71000000, "4m"},
    {144000000, 148000000, "2m"}, {420000000, 450000000, "70cm"},
};

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

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

/* A JSON string of printable ASCII without escapes: every accepted value is a call, grid, report or time. */
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

static bool valid_call(const char *s)
{
    const size_t n = strlen(s);
    bool letter = false, number = false;
    if (n < 3 || n > 11 || s[0] == '/' || s[n - 1] == '/') return false;
    for (; *s; ++s) {
        if (digit(*s)) number = true;
        else if (*s >= 'A' && *s <= 'Z') letter = true;
        else if (*s != '/') return false;
    }
    return letter && number;
}

static bool valid_grid(const char *s)
{
    return !*s || (strlen(s) == 4 && s[0] >= 'A' && s[0] <= 'R' &&
                   s[1] >= 'A' && s[1] <= 'R' && digit(s[2]) && digit(s[3]));
}

/* An FT8 report, -50 .. +49 dB, written back as WSJT-X logs it ("-07", "+12"). */
static bool parse_report(const char *s, char out[4])
{
    if ((s[0] != '-' && s[0] != '+') || !digit(s[1]) || (s[2] && (!digit(s[2]) || s[3]))) return false;
    const int value = atoi(s);
    if (value < -50 || value > 49) return false;
    snprintf(out, 4, "%+03d", value);
    return true;
}

static int two(const char *s)
{
    return (s[0] - '0') * 10 + (s[1] - '0');
}

static bool valid_time(const char *s)
{
    for (int i = 0; i < 14; ++i) {
        if (!digit(s[i])) return false;
    }
    if (s[14]) return false;
    const int year = two(s) * 100 + two(s + 2), month = two(s + 4), day = two(s + 6);
    static const int days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return year >= 2020 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 &&
           day <= days[month - 1] - (month == 2 && !leap) &&
           two(s + 8) < 24 && two(s + 10) < 60 && two(s + 12) < 60;
}

static bool parse_value(const char **p, unsigned field, qso_adif_t *qso)
{
    if (field == FREQ) {
        skip_space(p);
        const char *end = *p;
        while (digit(*end)) ++end;
        if (end == *p || end - *p > 9 || **p == '0') return false;
        qso->freq_hz = (uint32_t)strtoul(*p, NULL, 10);
        *p = end;
        return qso_adif_band(qso->freq_hz) != NULL;
    }
    char value[16];
    if (!read_string(p, value, sizeof(value))) return false;
    char *out;
    size_t size;
    bool ok;
    switch (field) {
    case CALL:   out = qso->call;     size = sizeof(qso->call);     ok = valid_call(value); break;
    case MYCALL: out = qso->my_call;  size = sizeof(qso->my_call);  ok = valid_call(value); break;
    case GRID:   out = qso->grid;     size = sizeof(qso->grid);     ok = valid_grid(value); break;
    case MYGRID: out = qso->my_grid;  size = sizeof(qso->my_grid);  ok = valid_grid(value); break;
    case ON:     out = qso->time_on;  size = sizeof(qso->time_on);  ok = valid_time(value); break;
    case OFF:    out = qso->time_off; size = sizeof(qso->time_off); ok = valid_time(value); break;
    case SENT:   return parse_report(value, qso->rst_sent);
    case RCVD:   return parse_report(value, qso->rst_rcvd);
    default:     return false;
    }
    if (!ok || strlen(value) >= size) return false;
    strcpy(out, value);
    return true;
}

bool qso_adif_parse(const char *json, qso_adif_t *qso)
{
    *qso = (qso_adif_t){0};
    unsigned present = 0;
    if (strlen(json) >= QSO_ADIF_JSON_BYTES || !take(&json, '{')) return false;
    do {
        char key[8];
        if (!read_string(&json, key, sizeof(key)) || !take(&json, ':')) return false;
        unsigned i = 0;
        while (i < FIELD_COUNT && strcmp(key, keys[i]) != 0) ++i;
        if (i == FIELD_COUNT || (present & (1u << i)) || !parse_value(&json, i, qso)) return false;
        present |= 1u << i;
    } while (take(&json, ','));
    if (!take(&json, '}')) return false;
    skip_space(&json);
    // The QSO ends after it starts; a log entry for our own call would be a mistake.
    return *json == '\0' && (present & REQUIRED) == REQUIRED &&
           strcmp(qso->time_off, qso->time_on) >= 0 && strcmp(qso->call, qso->my_call) != 0;
}

const char *qso_adif_band(uint32_t freq_hz)
{
    for (size_t i = 0; i < sizeof(bands) / sizeof(*bands); ++i) {
        if (freq_hz >= bands[i].low && freq_hz <= bands[i].high) return bands[i].name;
    }
    return NULL;
}

/* Append "<NAME:len>value " to out; false when it does not fit. */
static bool field(char *out, size_t capacity, size_t *used, const char *name, const char *value)
{
    if (!*value) return true;
    const int n = snprintf(out + *used, capacity - *used, "<%s:%u>%s ", name, (unsigned)strlen(value), value);
    if (n < 0 || (size_t)n >= capacity - *used) return false;
    *used += (size_t)n;
    return true;
}

int qso_adif_format(const qso_adif_t *qso, char *out, size_t capacity)
{
    const char *band = qso_adif_band(qso->freq_hz);
    if (!band || capacity == 0) return -1;
    char freq[16], date_on[9], date_off[9];
    snprintf(freq, sizeof(freq), "%u.%06u", (unsigned)(qso->freq_hz / 1000000), (unsigned)(qso->freq_hz % 1000000));
    snprintf(date_on, sizeof(date_on), "%.8s", qso->time_on);
    snprintf(date_off, sizeof(date_off), "%.8s", qso->time_off);
    size_t used = 0;
    const bool ok = field(out, capacity, &used, "CALL", qso->call) &&
                    field(out, capacity, &used, "GRIDSQUARE", qso->grid) &&
                    field(out, capacity, &used, "MODE", "FT8") &&
                    field(out, capacity, &used, "RST_SENT", qso->rst_sent) &&
                    field(out, capacity, &used, "RST_RCVD", qso->rst_rcvd) &&
                    field(out, capacity, &used, "QSO_DATE", date_on) &&
                    field(out, capacity, &used, "TIME_ON", qso->time_on + 8) &&
                    field(out, capacity, &used, "QSO_DATE_OFF", date_off) &&
                    field(out, capacity, &used, "TIME_OFF", qso->time_off + 8) &&
                    field(out, capacity, &used, "BAND", band) &&
                    field(out, capacity, &used, "FREQ", freq) &&
                    field(out, capacity, &used, "STATION_CALLSIGN", qso->my_call) &&
                    field(out, capacity, &used, "MY_GRIDSQUARE", qso->my_grid);
    const int n = ok ? snprintf(out + used, capacity - used, "<EOR>\n") : -1;
    if (n < 0 || (size_t)n >= capacity - used) return -1;
    return (int)(used + (size_t)n);
}

int qso_adif_header(const char *program_version, char *out, size_t capacity)
{
    static const char program[] = "FTDX10-FTX1-Network-Remote";
    const int n = snprintf(out, capacity,
                           "FT8 log from the FTDX10 / FTX-1 Network Remote\n"
                           "<ADIF_VER:5>3.1.4 <PROGRAMID:%u>%s <PROGRAMVERSION:%u>%s <EOH>\n",
                           (unsigned)strlen(program), program, (unsigned)strlen(program_version), program_version);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}
