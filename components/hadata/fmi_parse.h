// A stream parser for the city forecast of meteo.gov.ua (fmi.json, action getCityWeather). The answer has some
// 300 KB of hourly values: the parser takes the parts of the answer as they come and keeps only the values for
// the panels. As the site, it takes 03:00 for the night and 15:00 for the day of each date.
// The parser has no ESP-IDF dependency: a test on the computer can use it.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMI_DAYS        10      // the dates from the day of the request (the site has 10 days)
#define FMI_NIGHT_HOUR  3
#define FMI_DAY_HOUR    15

typedef struct {
    bool valid;
    int temp;           // °C, rounded
    int symbol;         // FMI SmartSymbol: 1..99 by day, 101..199 at night
} fmi_point_t;

typedef struct {
    int year, month, mday;      // the date
    fmi_point_t night;  // 03:00
    fmi_point_t day;    // 15:00
} fmi_day_t;

typedef struct {
    fmi_day_t days[FMI_DAYS];

    // The state of the stream.
    int depth;                  // the nesting of objects and arrays
    unsigned long arrays;       // bit n = 1: the level n + 1 is an array
    bool expect_key;            // in an object, the next string is a key
    bool in_string;
    bool escape;
    bool string_is_key;
    char text[24];              // the current string or scalar, cut to this size
    size_t text_len;
    bool in_scalar;
    char key[24];               // the last key of the current object
    char root_key[24];          // the last key of the root object
    bool in_list;               // in the array "fulldata"
    // The values of the current entry of "fulldata".
    char time[24];
    bool has_temp;
    double temp;
    bool has_symbol;
    int symbol;
} fmi_parser_t;

// Start a parser for FMI_DAYS dates from year-month-day (the local date of the request).
void fmi_parser_init(fmi_parser_t *p, int year, int month, int day);

// Give the next part of the answer.
void fmi_parser_feed(fmi_parser_t *p, const char *data, size_t len);

// Return the number of dates with a night or a day value.
int fmi_parser_count(const fmi_parser_t *p);

#ifdef __cplusplus
}
#endif
