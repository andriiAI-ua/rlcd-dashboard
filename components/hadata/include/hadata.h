// Data from Home Assistant and the meteo.gov.ua forecast, for the dashboard.
// A task reads the data once a minute. The logic is the same as in trmnl-server (DashboardDataService).
#pragma once

#include <stdbool.h>
#include <time.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    double value;
    bool has_change;    // the value of 24 hours ago is known
    double change;      // value now minus the value of 24 hours ago
} ha_sensor_t;

typedef struct {
    bool configured;            // the settings have the Home Assistant address and token
    bool ok;                    // the last request succeeded
    time_t updated;             // time of the last good data, 0 = never
    char error[64];             // the last error for the settings page, "" = none

    ha_sensor_t temperature;    // outdoor
    ha_sensor_t humidity;       // outdoor
    ha_sensor_t aqi;
    bool voltage_valid;
    double voltage;             // UPS input voltage

    bool alert_known;           // false: no alert entity answered (or no answer of ubilling.net.ua)
    bool alert_ubilling;        // the alerts come from ubilling.net.ua (no Home Assistant entities)
    time_t alert_at;            // ubilling.net.ua: the time of the last answer, 0 = none
    bool drone;
    bool missile;
    time_t alert_since;         // 0 = not known

    int dtek;                   // grid power: 1 = on, 0 = off, -1 = not known
    char hc_state[12];          // healthchecks.io check status ("up", "down", ...), "" = no check
    time_t hc_at;               // time of hc_state, 0 = not read yet
    int ping_dtek;              // the device without backup power: 1 = answers, 0 = no answer, -1
    time_t ping_at;             // time of the last ping, 0 = no ping yet
    int dtek_source;            // where dtek comes from: HADATA_DTEK_...
    int flat_powered;           // power in the flat (grid or generator): 1, 0, -1
    time_t next_off;            // 0 = not known or in the past
    time_t next_on;
    bool schedule_known;        // the schedule entities answered

    // Without an outdoor temperature (or humidity) of Home Assistant: the current weather of meteo.gov.ua.
    bool outdoor_meteo;         // temperature or humidity come from meteo.gov.ua
    time_t outdoor_at;          // the time of that observation

    bool forecast_valid;        // tomorrow
    bool forecast_has_temp;
    int forecast_temp;
    char forecast_text[64];
} ha_data_t;

typedef void (*hadata_change_cb_t)(void);

// Start the reading task. on_change runs after each reading. It runs in the task: keep it short.
void hadata_start(const settings_t *settings, hadata_change_cb_t on_change);

// Copy the last data.
void hadata_get(ha_data_t *out);

// Read the data now (for example, after Wi-Fi is on again in eco mode).
void hadata_refresh(void);

// Stop (true) or start again (false) the requests to Home Assistant, healthchecks.io, meteo.gov.ua and
// the ping. The "З сервера" screen stops them: trmnl-server gives the screen with the data.
void hadata_pause(bool paused);

// The cities of the forecast: meteo.gov.ua stations of the regional centres. The names come from the list
// "meteo.cities" of the language files, in the same order.
int hadata_city_count(void);
const char *hadata_city_station(int i);     // "33345"; "" for i out of the list
const char *hadata_city_name(int i);        // in the current language; "" for i out of the list
int hadata_city_find(const char *station);  // the index of a station, or -1
int hadata_city_find_name(const char *name);// the index of a city name (current language), or -1

// Use another meteo.gov.ua station ("" = no forecast) and read its forecast at the next chance.
void hadata_set_station(const char *station);

// The regions of the alerts of ubilling.net.ua. The device uses them when the Home Assistant alert entities are
// empty. The names come from the list "alerts.regions" of the language files, in the same order.
int hadata_region_count(void);
const char *hadata_region_key(int i);       // the key in the answer of ubilling.net.ua; "" for i out of the list
const char *hadata_region_name(int i);      // in the current language
int hadata_region_find(const char *key);    // the index of a key, or -1
int hadata_region_find_name(const char *name);  // the index of a name (current language), or -1

// Use another region ("" = no alerts from ubilling.net.ua) and read it at the next chance.
void hadata_set_region(const char *key);

// Return the number of completed readings. It changes after each reading while online.
unsigned hadata_read_count(void);

// Daylight saving time from the time zone of Home Assistant: the UTC offset now, the next change of the offset
// and the offset after it.
typedef struct {
    int offset;         // seconds east of UTC, now
    time_t next;        // the next change (UTC), 0 = no change in the next 400 days
    int next_offset;    // seconds east of UTC after the change
} hadata_tz_t;

// Ask Home Assistant for the time zone at the next chance: one request, also while the other requests stop.
void hadata_request_tz(void);

// Copy the last answer about the time zone. Return its number, 0 = no answer since the start.
unsigned hadata_get_tz(hadata_tz_t *out);

// The sources of the DTEK state, in their order.
#define HADATA_DTEK_NONE    0   // no source knows: the screen takes it as no DTEK power
#define HADATA_DTEK_HA      1   // the Home Assistant entity
#define HADATA_DTEK_HC      2   // the healthchecks.io check
#define HADATA_DTEK_PING    3   // the ping of the device without backup power

// The longest age of the data for the DTEK state: Home Assistant (default 5 min) and the check and
// the ping (at least 15 min). In eco mode the device reads only at the updates: the limits must be longer.
void hadata_set_max_age(int ha_seconds, int other_seconds);

#ifdef __cplusplus
}
#endif
