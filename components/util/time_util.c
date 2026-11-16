// Time helpers without the C library time zone (timegm is not available).
#include "time_util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

// Days since 1970-01-01 for a date of the proleptic Gregorian calendar.
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

time_t time_utc(int year, int month, int day, int hour, int minute, int second)
{
    return (time_t)(days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second);
}

// Read n digits. Return -1 if a character is not a digit.
static int digits(const char *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)p[i])) {
            return -1;
        }
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

bool time_parse_iso8601(const char *s, time_t *out)
{
    // "YYYY-MM-DDTHH:MM" is the minimum.
    const int y = digits(s, 4);
    if (y < 0 || s[4] != '-') return false;
    const int mo = digits(s + 5, 2);
    if (mo < 1 || mo > 12 || s[7] != '-') return false;
    const int d = digits(s + 8, 2);
    if (d < 1 || d > 31 || (s[10] != 'T' && s[10] != ' ')) return false;
    const int h = digits(s + 11, 2);
    if (h < 0 || h > 23 || s[13] != ':') return false;
    const int mi = digits(s + 14, 2);
    if (mi < 0 || mi > 59) return false;
    const char *p = s + 16;
    int sec = 0;
    if (*p == ':') {
        sec = digits(p + 1, 2);
        if (sec < 0 || sec > 60) return false;
        p += 3;
        if (*p == '.' || *p == ',') {
            p++;
            while (isdigit((unsigned char)*p)) {
                p++;
            }
        }
    }
    int offset = 0;
    if (*p == '+' || *p == '-') {
        const int oh = digits(p + 1, 2);
        if (oh < 0) return false;
        int om = 0;
        if (p[3] == ':') {
            om = digits(p + 4, 2);
        } else if (isdigit((unsigned char)p[3])) {
            om = digits(p + 3, 2);
        }
        if (om < 0) return false;
        offset = (oh * 60 + om) * 60 * (*p == '-' ? -1 : 1);
    }
    *out = time_utc(y, mo, d, h, mi, sec) - offset;
    return true;
}

void time_format_iso8601(time_t t, char *buf, int len)
{
    struct tm u;
    gmtime_r(&t, &u);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday,
             u.tm_hour, u.tm_min, u.tm_sec);
}
