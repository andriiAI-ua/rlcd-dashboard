// A stream parser for the city forecast of meteo.gov.ua: see fmi_parse.h.
//
// The answer is JSON: {"html": ..., "data": {...}, "dataDetailed": [...], "current": [...], "fulldata": [...], ...}.
// "fulldata" has one object for each hour from two days before the request, also for the past hours:
// {"name": "Vinnytsia", "time": "20261123T030000", "meantemp": 0, "SmartSymbol": 104, ...}. The time is local.
// A value that the site does not have is null.
#include "fmi_parse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_days(int y, int m)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap_year(y) ? 29 : days[m - 1];
}

void fmi_parser_init(fmi_parser_t *p, int year, int month, int day)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < FMI_DAYS; i++) {
        p->days[i].year = year;
        p->days[i].month = month;
        p->days[i].mday = day;
        if (++day > month_days(year, month)) {
            day = 1;
            if (++month > 12) {
                month = 1;
                year++;
            }
        }
    }
}

int fmi_parser_count(const fmi_parser_t *p)
{
    int n = 0;
    for (int i = 0; i < FMI_DAYS; i++) {
        if (p->days[i].night.valid || p->days[i].day.valid) {
            n++;
        }
    }
    return n;
}

static bool in_object(const fmi_parser_t *p)
{
    return p->depth > 0 && !(p->arrays & (1UL << (p->depth - 1)));
}

static void add_char(fmi_parser_t *p, char c)
{
    if (p->text_len + 1 < sizeof(p->text)) {
        p->text[p->text_len++] = c;
    }
}

// The end of an entry of "fulldata": keep its values if its time is 03:00 or 15:00 of a date of the list.
static void end_entry(fmi_parser_t *p)
{
    int y, m, d, h;
    if (!p->has_temp || sscanf(p->time, "%4d%2d%2dT%2d", &y, &m, &d, &h) != 4) {
        return;
    }
    if (h != FMI_NIGHT_HOUR && h != FMI_DAY_HOUR) {
        return;
    }
    for (int i = 0; i < FMI_DAYS; i++) {
        fmi_day_t *day = &p->days[i];
        if (day->year == y && day->month == m && day->mday == d) {
            fmi_point_t *pt = h == FMI_NIGHT_HOUR ? &day->night : &day->day;
            pt->valid = true;
            pt->temp = (int)floor(p->temp + 0.5);   // as Math.round() of the site
            pt->symbol = p->has_symbol ? p->symbol : 0;
            return;
        }
    }
}

static void on_value(fmi_parser_t *p, bool is_string)
{
    if (!p->in_list || p->depth != 3) {
        return;
    }
    if (is_string) {
        if (strcmp(p->key, "time") == 0) {
            strlcpy(p->time, p->text, sizeof(p->time));
        }
        return;
    }
    if (strcmp(p->text, "null") == 0) {
        return;
    }
    char *end;
    const double v = strtod(p->text, &end);
    if (end == p->text) {
        return;
    }
    if (strcmp(p->key, "meantemp") == 0) {
        p->has_temp = true;
        p->temp = v;
    } else if (strcmp(p->key, "SmartSymbol") == 0) {
        p->has_symbol = true;
        p->symbol = (int)v;
    }
}

static void open_level(fmi_parser_t *p, bool is_array)
{
    if (p->depth >= (int)(sizeof(p->arrays) * 8)) {
        return;
    }
    p->depth++;
    if (is_array) {
        p->arrays |= 1UL << (p->depth - 1);
    } else {
        p->arrays &= ~(1UL << (p->depth - 1));
    }
    p->expect_key = !is_array;
    if (p->depth == 2 && is_array && strcmp(p->root_key, "fulldata") == 0) {
        p->in_list = true;
    }
    if (p->in_list && p->depth == 3 && !is_array) {
        p->time[0] = '\0';
        p->has_temp = false;
        p->has_symbol = false;
    }
}

static void close_level(fmi_parser_t *p)
{
    if (p->depth == 0) {
        return;
    }
    if (p->in_list && p->depth == 3) {
        end_entry(p);
    }
    if (p->in_list && p->depth == 2) {
        p->in_list = false;
    }
    p->depth--;
    p->expect_key = false;
}

static void end_string(fmi_parser_t *p)
{
    p->text[p->text_len] = '\0';
    if (p->string_is_key) {
        strlcpy(p->key, p->text, sizeof(p->key));
        if (p->depth == 1) {
            strlcpy(p->root_key, p->text, sizeof(p->root_key));
        }
        p->expect_key = false;
    } else {
        on_value(p, true);
    }
}

static bool scalar_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '+' ||
           c == '.';
}

void fmi_parser_feed(fmi_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (p->in_string) {
            if (p->escape) {
                p->escape = false;
                add_char(p, c);
            } else if (c == '\\') {
                p->escape = true;
            } else if (c == '"') {
                p->in_string = false;
                end_string(p);
            } else {
                add_char(p, c);
            }
            continue;
        }
        if (p->in_scalar) {
            if (scalar_char(c)) {
                add_char(p, c);
                continue;
            }
            p->in_scalar = false;
            p->text[p->text_len] = '\0';
            on_value(p, false);
        }
        switch (c) {
        case '"':
            p->in_string = true;
            p->text_len = 0;
            p->string_is_key = in_object(p) && p->expect_key;
            break;
        case '{':
            open_level(p, false);
            break;
        case '[':
            open_level(p, true);
            break;
        case '}':
        case ']':
            close_level(p);
            break;
        case ':':
            p->expect_key = false;
            break;
        case ',':
            p->expect_key = in_object(p);
            break;
        default:
            if (scalar_char(c)) {
                p->in_scalar = true;
                p->text_len = 0;
                add_char(p, c);
            }
            break;
        }
    }
}
