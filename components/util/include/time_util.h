// Time helpers that do not depend on the time zone.
#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Return the Unix time of a UTC date and time (proleptic Gregorian calendar).
time_t time_utc(int year, int month, int day, int hour, int minute, int second);

// Read an ISO 8601 time as Home Assistant writes it:
// "2026-11-11T16:30:00+00:00", "2026-11-11T16:30:00.123456+02:00", "2026-11-11 16:30:00Z".
// A time without an offset is UTC. Return false if the text is not such a time.
bool time_parse_iso8601(const char *text, time_t *out);

// Write a UTC time as "2026-11-11T16:30:00Z".
void time_format_iso8601(time_t t, char *buf, int len);

#ifdef __cplusplus
}
#endif
