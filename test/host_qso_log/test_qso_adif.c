/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "qso_adif.h"

#define BASE "\"call\":\"YO3ABC\",\"sent\":\"-10\",\"rcvd\":\"+5\",\"on\":\"20261127153000\"," \
             "\"off\":\"20261127153145\",\"mycall\":\"YO9VA\""

static bool parses(const char *json)
{
    qso_adif_t qso;
    return qso_adif_parse(json, &qso);
}

/* BASE with one more field, or with a field of BASE replaced by `extra` when it names the same key */
static bool with(const char *extra)
{
    char json[QSO_ADIF_JSON_BYTES * 2];
    snprintf(json, sizeof(json), "{\"freq\":14075500,%s}", extra);
    return parses(json);
}

static bool with_base(const char *extra)
{
    char json[QSO_ADIF_JSON_BYTES * 2];
    snprintf(json, sizeof(json), "{" BASE ",%s}", extra);
    return parses(json);
}

int main(void)
{
    qso_adif_t qso;
    const char *full = "{ \"call\":\"YO3ABC\", \"grid\":\"KN34\", \"sent\":\"-10\", \"rcvd\":\"+5\", \"freq\":14075500,"
                       " \"on\":\"20261127153000\", \"off\":\"20261127153145\", \"mycall\":\"YO9VA\", \"mygrid\":\"KN37\" }";
    assert(qso_adif_parse(full, &qso));
    assert(strcmp(qso.rst_rcvd, "+05") == 0 && strcmp(qso.rst_sent, "-10") == 0);
    char record[QSO_ADIF_RECORD_BYTES];
    const int n = qso_adif_format(&qso, record, sizeof(record));
    const char *expected =
        "<CALL:6>YO3ABC <GRIDSQUARE:4>KN34 <MODE:3>FT8 <RST_SENT:3>-10 <RST_RCVD:3>+05 "
        "<QSO_DATE:8>20261127 <TIME_ON:6>153000 <QSO_DATE_OFF:8>20261127 <TIME_OFF:6>153145 "
        "<BAND:3>20m <FREQ:9>14.075500 <STATION_CALLSIGN:5>YO9VA <MY_GRIDSQUARE:4>KN37 <EOR>\n";
    assert(n == (int)strlen(expected) && strcmp(record, expected) == 0);
    assert(qso_adif_format(&qso, record, (size_t)n) == -1);   // no room for the NUL
    assert(qso_adif_format(&qso, record, 0) == -1);

    // Optional grids are left out of the record
    assert(qso_adif_parse("{" BASE ",\"freq\":7074000,\"grid\":\"\"}", &qso));
    assert(qso_adif_format(&qso, record, sizeof(record)) > 0);
    assert(!strstr(record, "GRIDSQUARE") && strstr(record, "<BAND:3>40m <FREQ:8>7.074000 "));

    // Required fields
    const char *required[] = {"call", "sent", "rcvd", "freq", "on", "off", "mycall"};
    for (size_t i = 0; i < sizeof(required) / sizeof(*required); ++i) {
        char json[QSO_ADIF_JSON_BYTES];
        snprintf(json, sizeof(json), "{" BASE ",\"freq\":14075500}");
        char key[16];
        snprintf(key, sizeof(key), "\"%s\":", required[i]);
        char *at = strstr(json, key);
        assert(at);
        char *end = strpbrk(at + strlen(key), ",}");
        if (*end == ',') ++end;
        else if (at[-1] == ',') --at;
        memmove(at, end, strlen(end) + 1);
        assert(!parses(json));
    }

    // Field checks
    assert(with_base("\"freq\":1800000") && with_base("\"freq\":450000000") && with_base("\"freq\":50313000"));
    assert(!with_base("\"freq\":1799999") && !with_base("\"freq\":11000000") && !with_base("\"freq\":0") &&
           !with_base("\"freq\":-14074000") && !with_base("\"freq\":\"14074000\"") &&
           !with_base("\"freq\":014074000") && !with_base("\"freq\":1234567890"));
    assert(with_base("\"freq\":14074000,\"grid\":\"RR99\"") && !with_base("\"freq\":14074000,\"grid\":\"SS00\"") &&
           !with_base("\"freq\":14074000,\"grid\":\"KN3\"") && !with_base("\"freq\":14074000,\"grid\":\"kn34\""));
    assert(!with("\"call\":\"YO3ABC\",\"call\":\"YO3ABC\"," BASE));   // duplicate key
    assert(!with_base("\"freq\":14074000,\"mode\":\"FT8\""));          // unknown key
    const char *bad_calls[] = {"\"AB\"", "\"ABCDEF\"", "\"123\"", "\"/YO3A\"", "\"YO3A/\"", "\"yo3abc\"",
                               "\"YO3ABCDEFGHI\"", "\"YO3<A>\"", "\"YO3\\\"A\"", "\"YO9VA\""};
    for (size_t i = 0; i < sizeof(bad_calls) / sizeof(*bad_calls); ++i) {
        char json[QSO_ADIF_JSON_BYTES];
        snprintf(json, sizeof(json), "{\"call\":%s,\"sent\":\"-10\",\"rcvd\":\"+5\",\"freq\":14074000,"
                 "\"on\":\"20261127153000\",\"off\":\"20261127153145\",\"mycall\":\"YO9VA\"}", bad_calls[i]);
        assert(!parses(json));
    }
    assert(parses("{\"call\":\"YO3ABC/P\",\"sent\":\"+49\",\"rcvd\":\"-50\",\"freq\":14074000,"
                  "\"on\":\"20240229235959\",\"off\":\"20240229235959\",\"mycall\":\"YO9VA\"}"));
    const char *bad_reports[] = {"\"+50\"", "\"-51\"", "\"10\"", "\"R-10\"", "\"-1a\"", "\"-100\"", "\"\"", "-10"};
    for (size_t i = 0; i < sizeof(bad_reports) / sizeof(*bad_reports); ++i) {
        char json[QSO_ADIF_JSON_BYTES];
        snprintf(json, sizeof(json), "{\"call\":\"YO3ABC\",\"sent\":%s,\"rcvd\":\"+5\",\"freq\":14074000,"
                 "\"on\":\"20261127153000\",\"off\":\"20261127153145\",\"mycall\":\"YO9VA\"}", bad_reports[i]);
        assert(!parses(json));
    }
    const char *bad_times[] = {"20230229120000", "20261327120000", "20261100120000", "20261127240000",
                               "20261127126000", "20261127120060", "2026112712000", "202611271200000",
                               "19991231235959", "2026-11-27T12:0"};
    for (size_t i = 0; i < sizeof(bad_times) / sizeof(*bad_times); ++i) {
        char json[QSO_ADIF_JSON_BYTES];
        snprintf(json, sizeof(json), "{\"call\":\"YO3ABC\",\"sent\":\"-10\",\"rcvd\":\"+5\",\"freq\":14074000,"
                 "\"on\":\"%s\",\"off\":\"20261127153145\",\"mycall\":\"YO9VA\"}", bad_times[i]);
        assert(!parses(json));
    }
    // The QSO cannot end before it starts
    assert(!parses("{\"call\":\"YO3ABC\",\"sent\":\"-10\",\"rcvd\":\"+5\",\"freq\":14074000,"
                   "\"on\":\"20261127153145\",\"off\":\"20261127153000\",\"mycall\":\"YO9VA\"}"));
    // Syntax
    assert(!parses("") && !parses("{}") && !parses("[]"));
    assert(!with_base("\"freq\":14074000}") && !with_base("\"freq\":14074000,"));
    {
        char json[QSO_ADIF_JSON_BYTES + 8];
        snprintf(json, sizeof(json), "{" BASE ",\"freq\":14074000}");
        const size_t len = strlen(json);
        memset(json + len, ' ', QSO_ADIF_JSON_BYTES - len);
        json[QSO_ADIF_JSON_BYTES] = '\0';
        assert(!parses(json));   // too long, even if only white space
        json[QSO_ADIF_JSON_BYTES - 1] = '\0';
        assert(parses(json));
    }

    assert(strcmp(qso_adif_band(14074000), "20m") == 0 && strcmp(qso_adif_band(432000000), "70cm") == 0);
    assert(qso_adif_band(29700001) == NULL && qso_adif_band(0) == NULL);

    char header[160];
    const int h = qso_adif_header("1.9.64-yo9va", header, sizeof(header));
    assert(h > 0 && strstr(header, "<ADIF_VER:5>3.1.4 ") && strstr(header, "<PROGRAMVERSION:12>1.9.64-yo9va <EOH>\n"));
    assert(qso_adif_header("1.9.64-yo9va", header, (size_t)h) == -1);

    puts("host_qso_log: all tests passed");
    return 0;
}
