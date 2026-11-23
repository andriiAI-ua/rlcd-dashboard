// Reads the dashboard data:
// - Home Assistant: one POST /api/template a minute gives all entities (state, times, unit) at once;
// - Home Assistant history: the values of 24 hours ago, one entity a minute, kept for 15 minutes;
// - meteo.gov.ua: the forecast informer, once an hour; the city forecast (fmi.json, the data of the forecast
//   page of the site) twice a day.
// The logic and the intervals are the same as in trmnl-server (DashboardDataService, HomeAssistantClient,
// MeteoForecastClient).
#include "hadata.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "texts.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "net.h"
#include "ping/ping_sock.h"
#include "time_util.h"
#include "fmi_parse.h"

static const char *TAG = "hadata";

#define MAX_LIST            4       // entities in one list
#define ENTITY_LEN          96
#define READ_PERIOD_MS      60000
#define OFFLINE_PERIOD_MS   5000
#define STALE_S             (30 * 60)   // a sensor value older than this is stale
#define GRID_ON_VOLTAGE     100.0       // a lower UPS input voltage means no power
#define VALID_TIME          1704067200  // 2024-01-01: before this the clock is not set

#define DAYAGO_REFRESH_S    (15 * 60)
#define DAYAGO_RETRY_S      (5 * 60)
#define DAYAGO_MAX_AGE_S    (60 * 60)
#define HISTORY_WINDOW_S    (60 * 60)

#define FORECAST_REFRESH_S  (60 * 60)
#define WEATHER_REFRESH_S   (30 * 60)       // the informer, when its current weather replaces Home Assistant
#define WEATHER_MAX_AGE_S   (3 * 60 * 60)   // an older observation is not shown
#define FORECAST_RETRY_S    (15 * 60)
#define FORECAST_MAX_AGE_S  (12 * 60 * 60)

// The city forecast: after 06:00 and after 18:00 on all screens.
#define DAYS_RETRY_S        (10 * 60)
#define DAYS_MAX_AGE_S      (36 * 60 * 60)  // the forecast of the dates stays useful
#define DAYS_MORNING_HOUR   6
#define DAYS_EVENING_HOUR   18
#define DAYS_URL_LEN        448

// healthchecks.io: an HTTPS request costs more energy than a request to Home Assistant on the home
// network, and the UPS status already tells about a power loss at once. So read the check less often.
#define HC_PERIOD_S         (5 * 60)
#define HC_MAX_AGE_S        (15 * 60)
#define HA_MAX_AGE_S        (5 * 60)    // older Home Assistant data does not give the DTEK state

// Ping of a device without backup power, at each reading: three pings, one answer is sufficient.
#define PING_COUNT          3
#define PING_INTERVAL_MS    500
#define PING_TIMEOUT_MS     1000

#define MAX_BODY            (96 * 1024)
#define TEMPLATE_CAP        4096

typedef struct {
    char id[MAX_LIST][ENTITY_LEN];
    int count;
} entity_list_t;

typedef struct {
    char state[64];
    char unit[16];
    time_t changed;
    time_t reported;
} entity_state_t;

typedef struct {
    char entity[ENTITY_LEN];
    bool has;
    double value;
    time_t value_at;
    time_t next_read;
} dayago_t;

typedef struct {
    int day;
    int month;
    bool has_temp;
    int temp;
    char text[64];
} forecast_day_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} body_t;

static char s_url[96];
static char s_token[256];
static char s_bearer[300];          // "Bearer <token>"
static char s_station[16];
static char s_region[64];           // the region of ubilling.net.ua, "" = none
static int s_ub_level = -1;         // ubilling.net.ua: 0 = no alert, 1 = yellow (drone), 2 = red (missile), -1
static time_t s_ub_since;           // the start of the alert (UTC), 0 = not known
static time_t s_ub_at;              // the time of the last answer, 0 = none
static char s_hc_key[64];
static char s_hc_check[64];
static int s_hc_dtek = -1;          // from the healthchecks.io check: 1 = up, 0 = down, -1 = not known
static char s_hc_state[12];         // the last check status, for the settings page
static time_t s_hc_at;              // time of s_hc_dtek
static volatile int s_hc_max_age = HC_MAX_AGE_S;
static volatile int s_ha_max_age = HA_MAX_AGE_S;
static char s_ping_host[64];         // a device without backup power: it answers only with DTEK power
static int s_ping_dtek = -1;        // 1 = it answers, 0 = no answer, -1 = not known
static time_t s_ping_at;            // time of s_ping_dtek
static entity_list_t s_temp, s_hum, s_aqi, s_volt, s_ups, s_drone, s_missile, s_off, s_on, s_dtek;
static char *s_template;
static body_t s_body;

static dayago_t s_dayago[3 * MAX_LIST];
static int s_dayago_count;

static forecast_day_t s_days[6];
static int s_day_count;
// The city forecast for the 5-panel and the 10-day screens.
static fmi_day_t s_fmi_days[FMI_DAYS];
static time_t s_fmi_at;
static time_t s_fmi_next;
static fmi_parser_t s_fmi_parser;
static volatile unsigned s_days_reads;
static time_t s_forecast_at;
// The current weather of the informer: the temperature, the humidity (-1 = none) and the observation time.
static bool s_now_valid;
static int s_now_temp;
static int s_now_hum = -1;
static time_t s_now_at;
static time_t s_forecast_next;

static ha_data_t s_data;            // Home Assistant part; hadata_get() adds the forecast
static SemaphoreHandle_t s_lock;
static hadata_change_cb_t s_on_change;
static TaskHandle_t s_task;
static volatile unsigned s_reads;   // completed readings while online

/* Helpers */

static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

// Copy a comma-separated list of entity ids. Keep only the characters that an entity id can have.
static void parse_list(const char *text, entity_list_t *list)
{
    list->count = 0;
    const char *p = text;
    while (*p && list->count < MAX_LIST) {
        char id[ENTITY_LEN];
        int n = 0;
        while (*p && *p != ',') {
            const char c = (char)tolower((unsigned char)*p);
            if ((isalnum((unsigned char)c) || c == '_' || c == '.') && n < ENTITY_LEN - 1) {
                id[n++] = c;
            }
            p++;
        }
        id[n] = '\0';
        if (*p == ',') {
            p++;
        }
        if (n > 0 && strchr(id, '.')) {
            strlcpy(list->id[list->count++], id, ENTITY_LEN);
        }
    }
}

static void append(char *buf, size_t cap, const char *text)
{
    strlcat(buf, text, cap);
}

static void append_ids(char *buf, size_t cap, const entity_list_t *list, bool *first)
{
    for (int i = 0; i < list->count; i++) {
        append(buf, cap, *first ? "'" : ",'");
        append(buf, cap, list->id[i]);
        append(buf, cap, "'");
        *first = false;
    }
}

// The template gives a JSON list: [[entity_id, state, last_changed, last_reported, unit], ...].
// expand() skips an entity that does not exist.
static char *make_template(void)
{
    char *t = calloc(1, TEMPLATE_CAP);
    if (!t) {
        return NULL;
    }
    append(t, TEMPLATE_CAP, "{%- set ns = namespace(o=[]) -%}{%- for s in expand([");
    bool first = true;
    const entity_list_t *lists[] = { &s_temp, &s_hum, &s_aqi, &s_volt, &s_ups, &s_drone, &s_missile,
                                     &s_off, &s_on, &s_dtek };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        append_ids(t, TEMPLATE_CAP, lists[i], &first);
    }
    append(t, TEMPLATE_CAP,
           "]) -%}{%- set ns.o = ns.o + [[s.entity_id, s.state, "
           "as_timestamp(s.last_changed, 0) | int, "
           "as_timestamp(s.last_reported | default(s.last_updated), 0) | int, "
           "s.attributes.unit_of_measurement | default('')]] -%}"
           "{%- endfor -%}{{ ns.o | tojson }}");
    return t;
}

/* HTTP */

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        body_t *b = e->user_data;
        if (b->len + (size_t)e->data_len + 1 > b->cap) {
            b->overflow = true;
            return ESP_OK;
        }
        memcpy(b->buf + b->len, e->data, e->data_len);
        b->len += (size_t)e->data_len;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

// Do a GET (post_body is NULL) or a POST with JSON. key_header and key_value give the access key
// (both NULL: no key). Return the HTTP status, or -1 if the request failed.
static int http_request(const char *url, const char *post_body, const char *key_header, const char *key_value)
{
    s_body.len = 0;
    s_body.overflow = false;
    s_body.buf[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .method = post_body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = on_http_event,
        .user_data = &s_body,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    if (key_header && key_value) {
        esp_http_client_set_header(c, key_header, key_value);
    }
    if (post_body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, post_body, (int)strlen(post_body));
    }
    const esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Request failed: %s", esp_err_to_name(err));
    }
    if (s_body.overflow) {
        ESP_LOGW(TAG, "The answer is larger than %d bytes", MAX_BODY);
        return -1;
    }
    return status;
}

/* Values */

static bool parse_number(const char *text, double *out)
{
    char *end;
    const double v = strtod(text, &end);
    if (end == text || !isfinite(v)) {
        return false;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static bool find_state(cJSON *root, const char *id, entity_state_t *out)
{
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        const cJSON *e = cJSON_GetArrayItem(item, 0);
        if (!cJSON_IsString(e) || strcmp(e->valuestring, id) != 0) {
            continue;
        }
        const cJSON *st = cJSON_GetArrayItem(item, 1);
        const cJSON *ch = cJSON_GetArrayItem(item, 2);
        const cJSON *rp = cJSON_GetArrayItem(item, 3);
        const cJSON *un = cJSON_GetArrayItem(item, 4);
        strlcpy(out->state, cJSON_IsString(st) ? st->valuestring : "", sizeof(out->state));
        strlcpy(out->unit, cJSON_IsString(un) ? un->valuestring : "", sizeof(out->unit));
        out->changed = cJSON_IsNumber(ch) ? (time_t)ch->valuedouble : 0;
        out->reported = cJSON_IsNumber(rp) ? (time_t)rp->valuedouble : out->changed;
        return true;
    }
    return false;
}

// Return the number of an entity. Return false if the entity is unavailable, not a number or stale.
static bool number_of(cJSON *root, const char *id, time_t now, double *out)
{
    entity_state_t st;
    if (!find_state(root, id, &st) || !parse_number(st.state, out)) {
        return false;
    }
    if (st.reported > 0 && now - st.reported > STALE_S) {
        ESP_LOGI(TAG, "%s is stale", id);
        return false;
    }
    return true;
}

static bool dayago_value(const char *id, time_t now, double *out)
{
    for (int i = 0; i < s_dayago_count; i++) {
        const dayago_t *e = &s_dayago[i];
        if (strcmp(e->entity, id) == 0 && e->has && now - e->value_at <= DAYAGO_MAX_AGE_S) {
            *out = e->value;
            return true;
        }
    }
    return false;
}

// The first entity with a number gives the value. The value of 24 hours ago comes from the same entity;
// if it has no history, from the other entities of the list.
static void read_sensor(cJSON *root, const entity_list_t *list, time_t now, ha_sensor_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < list->count; i++) {
        double v;
        if (!number_of(root, list->id[i], now, &v)) {
            continue;
        }
        out->valid = true;
        out->value = v;
        double old;
        bool found = dayago_value(list->id[i], now, &old);
        for (int j = 0; j < list->count && !found; j++) {
            if (j != i) {
                found = dayago_value(list->id[j], now, &old);
            }
        }
        if (found) {
            out->has_change = true;
            out->change = v - old;
        }
        return;
    }
}

// Read "1d 2h 3m", "0h 12m", "45m", "2 h 5 min". Each part is optional, the order is d, h, m.
static bool parse_duration(const char *s, double *seconds)
{
    long d = 0, h = 0, m = 0;
    int last = 0;
    bool any = false;
    const char *p = s;
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        char *end;
        const long n = strtol(p, &end, 10);
        p = end;
        while (*p == ' ') {
            p++;
        }
        const char u = (char)tolower((unsigned char)*p);
        if (u == 'd' && last < 1) {
            d = n;
            last = 1;
            p++;
        } else if (u == 'h' && last < 2) {
            h = n;
            last = 2;
            p++;
        } else if (u == 'm' && last < 3) {
            m = n;
            last = 3;
            p++;
            if (tolower((unsigned char)p[0]) == 'i' && tolower((unsigned char)p[1]) == 'n') {
                p += 2;
            }
        } else {
            return false;
        }
        any = true;
    }
    if (!any) {
        return false;
    }
    *seconds = d * 86400.0 + h * 3600.0 + m * 60.0;
    return true;
}

// Get the time of the next power change from an entity state (see ParseScheduleTime in trmnl-server).
// The state can be a timestamp, a number with a unit of time ("6.8167" h), or a text ("0h 12m").
// A number without a unit is minutes. A time to wait starts when the state changed.
// The result goes to the nearest 15 minutes. Return 0 if the state has no time.
static time_t schedule_time(const char *state, const char *unit, time_t changed, time_t now)
{
    char text[64];
    const char *p = state;
    while (*p == ' ') {
        p++;
    }
    strlcpy(text, p, sizeof(text));
    for (int n = (int)strlen(text); n > 0 && text[n - 1] == ' '; n--) {
        text[n - 1] = '\0';
    }
    if (text[0] == '\0') {
        return 0;
    }

    double wait;
    double number;
    if (parse_number(text, &number)) {
        double mult = 60;
        if (strcasecmp(unit, "d") == 0) {
            mult = 86400;
        } else if (strcasecmp(unit, "h") == 0) {
            mult = 3600;
        } else if (strcasecmp(unit, "s") == 0) {
            mult = 1;
        } else if (strcasecmp(unit, "ms") == 0) {
            mult = 0.001;
        }
        wait = number * mult;
    } else if (!parse_duration(text, &wait)) {
        time_t t;
        return time_parse_iso8601(text, &t) ? t : 0;
    }
    const time_t target = (changed > 0 ? changed : now) + (time_t)llround(wait);
    return (target + 450) / 900 * 900;
}

static time_t future_time(cJSON *root, const entity_list_t *list, time_t now, bool *found)
{
    entity_state_t st;
    if (list->count == 0 || !find_state(root, list->id[0], &st)) {
        return 0;
    }
    *found = true;
    const time_t t = schedule_time(st.state, st.unit, st.changed, now);
    return t > now ? t : 0;
}

// NUT status: "OL", "OL CHRG", "OB DISCHRG LB". Return 1 for input power, 0 for battery, -1 if not known.
static int ups_has_power(const char *state)
{
    char buf[64];
    strlcpy(buf, state, sizeof(buf));
    bool ol = false, ob = false;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (strcasecmp(tok, "OB") == 0) {
            ob = true;
        } else if (strcasecmp(tok, "OL") == 0) {
            ol = true;
        }
    }
    return ob ? 0 : (ol ? 1 : -1);
}

static int on_off(const char *state)
{
    if (strcasecmp(state, "on") == 0 || strcasecmp(state, "true") == 0 || strcasecmp(state, "up") == 0) {
        return 1;
    }
    if (strcasecmp(state, "off") == 0 || strcasecmp(state, "false") == 0 || strcasecmp(state, "down") == 0) {
        return 0;
    }
    return -1;
}

static void compute(cJSON *root, time_t now, ha_data_t *d)
{
    read_sensor(root, &s_temp, now, &d->temperature);
    read_sensor(root, &s_hum, now, &d->humidity);
    read_sensor(root, &s_aqi, now, &d->aqi);

    for (int i = 0; i < s_volt.count && !d->voltage_valid; i++) {
        d->voltage_valid = number_of(root, s_volt.id[i], now, &d->voltage);
    }

    // Alerts. The alert starts at the earliest change of an active entity.
    entity_state_t drone, missile;
    const bool has_drone = s_drone.count > 0 && find_state(root, s_drone.id[0], &drone);
    const bool has_missile = s_missile.count > 0 && find_state(root, s_missile.id[0], &missile);
    d->alert_known = has_drone || has_missile;
    d->drone = has_drone && on_off(drone.state) == 1;
    d->missile = has_missile && on_off(missile.state) == 1;
    d->alert_since = 0;
    if (d->drone && drone.changed > 0) {
        d->alert_since = drone.changed;
    }
    if (d->missile && missile.changed > 0 && (d->alert_since == 0 || missile.changed < d->alert_since)) {
        d->alert_since = missile.changed;
    }

    // Power in the flat: the UPS status, else the UPS input voltage.
    entity_state_t st;
    d->flat_powered = -1;
    if (s_ups.count > 0 && find_state(root, s_ups.id[0], &st)) {
        d->flat_powered = ups_has_power(st.state);
    }
    if (d->flat_powered < 0 && d->voltage_valid) {
        d->flat_powered = d->voltage >= GRID_ON_VOLTAGE ? 1 : 0;
    }
    d->dtek = -1;
    if (s_dtek.count > 0 && find_state(root, s_dtek.id[0], &st)) {
        d->dtek = on_off(st.state);
    }

    bool known = false;
    d->next_off = future_time(root, &s_off, now, &known);
    d->next_on = future_time(root, &s_on, now, &known);
    d->schedule_known = known;
}

/* Home Assistant */

static void set_error(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data.ok = false;
    strlcpy(s_data.error, text, sizeof(s_data.error));
    xSemaphoreGive(s_lock);
}

static void read_states(time_t now)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "template", s_template);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return;
    }
    char url[160];
    snprintf(url, sizeof(url), "%s/api/template", s_url);
    const int status = http_request(url, body, "Authorization", s_bearer);
    cJSON_free(body);

    char error[64];
    if (status != 200) {
        if (status == 401) {
            strlcpy(error, tr(T_DATA_BAD_TOKEN), sizeof(error));
        } else if (status < 0) {
            strlcpy(error, tr(T_DATA_NO_LINK), sizeof(error));
        } else {
            snprintf(error, sizeof(error), "HA: HTTP %d", status);
        }
        ESP_LOGW(TAG, "%s", error);
        set_error(error);
        return;
    }
    cJSON *root = cJSON_Parse(s_body.buf);
    if (!cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "Unexpected answer: %.200s", s_body.buf);
        cJSON_Delete(root);
        set_error(tr(T_DATA_BAD_ANSWER));
        return;
    }
    ha_data_t d = { 0 };
    compute(root, now, &d);
    cJSON_Delete(root);

    d.configured = true;
    d.ok = true;
    d.updated = now;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_lock);
}

// Read the value of one entity 24 hours ago: look 1 hour before and after that time and take
// the last number before it, else the first number after it.
static void read_dayago(dayago_t *e, time_t now)
{
    const time_t at = now - 24 * 3600;
    char start[24], end[24], end_query[40];
    time_format_iso8601(at - HISTORY_WINDOW_S, start, sizeof(start));
    time_format_iso8601(at + HISTORY_WINDOW_S, end, sizeof(end));
    // end_time is in the query: write ':' as %3A.
    size_t n = 0;
    for (const char *p = end; *p && n + 4 < sizeof(end_query); p++) {
        if (*p == ':') {
            memcpy(end_query + n, "%3A", 3);
            n += 3;
        } else {
            end_query[n++] = *p;
        }
    }
    end_query[n] = '\0';

    char url[400];
    snprintf(url, sizeof(url),
             "%s/api/history/period/%s?filter_entity_id=%s&end_time=%s&minimal_response&no_attributes",
             s_url, start, e->entity, end_query);
    const int status = http_request(url, NULL, "Authorization", s_bearer);
    if (status != 200) {
        ESP_LOGW(TAG, "History of %s: HTTP %d", e->entity, status);
        e->next_read = now + DAYAGO_RETRY_S;
        return;
    }

    cJSON *root = cJSON_Parse(s_body.buf);
    bool have_before = false, have_after = false;
    time_t before_t = 0, after_t = 0;
    double before_v = 0, after_v = 0;
    const time_t from = at - HISTORY_WINDOW_S;
    cJSON *series;
    cJSON_ArrayForEach(series, root) {
        cJSON *item;
        cJSON_ArrayForEach(item, series) {
            const cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "state");
            double v;
            if (!cJSON_IsString(st) || !parse_number(st->valuestring, &v)) {
                continue;
            }
            // The first entry is the state at the start of the window; its change can be earlier.
            const cJSON *lc = cJSON_GetObjectItemCaseSensitive(item, "last_changed");
            time_t t;
            if (!cJSON_IsString(lc) || !time_parse_iso8601(lc->valuestring, &t)) {
                t = from;
            }
            if (t <= at) {
                if (!have_before || t >= before_t) {
                    have_before = true;
                    before_t = t;
                    before_v = v;
                }
            } else if (!have_after || t < after_t) {
                have_after = true;
                after_t = t;
                after_v = v;
            }
        }
    }
    cJSON_Delete(root);

    if (have_before || have_after) {
        e->has = true;
        e->value = have_before ? before_v : after_v;
        e->value_at = now;
        e->next_read = now + DAYAGO_REFRESH_S;
    } else {
        ESP_LOGI(TAG, "No value of 24 hours ago for %s", e->entity);
        e->next_read = now + DAYAGO_RETRY_S;
    }
}

static void read_one_dayago(time_t now)
{
    for (int i = 0; i < s_dayago_count; i++) {
        if (now >= s_dayago[i].next_read) {
            read_dayago(&s_dayago[i], now);
            return;
        }
    }
}

/* Forecast */

static const char *after_tag(const char *p, const char *tag)
{
    const char *q = p ? strstr(p, tag) : NULL;
    return q ? q + strlen(tag) : NULL;
}

// Copy n characters and replace the HTML entities that the informer uses.
static void html_decode(const char *in, size_t n, char *out, size_t cap)
{
    static const struct {
        const char *name;
        const char *text;
    } entities[] = {
        { "&deg;", "°" }, { "&nbsp;", " " }, { "&amp;", "&" }, { "&quot;", "\"" }, { "&#39;", "'" },
        { "&lt;", "<" }, { "&gt;", ">" }, { "&minus;", "−" },
    };
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap;) {
        bool done = false;
        if (in[i] == '&') {
            for (size_t k = 0; k < sizeof(entities) / sizeof(entities[0]); k++) {
                const size_t len = strlen(entities[k].name);
                if (i + len <= n && strncmp(in + i, entities[k].name, len) == 0) {
                    const size_t tl = strlen(entities[k].text);
                    memcpy(out + o, entities[k].text, tl);
                    o += tl;
                    i += len;
                    done = true;
                    break;
                }
            }
        }
        if (!done) {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ') {
        p++;
    }
    memmove(s, p, strlen(p) + 1);
    for (int n = (int)strlen(s); n > 0 && s[n - 1] == ' '; n--) {
        s[n - 1] = '\0';
    }
}

// The informer is a script with HTML. Each day has a date (dd.MM), an icon with a description
// in its title, and a temperature.
static int parse_forecast(const char *script, forecast_day_t *days, int max)
{
    int count = 0;
    const char *p = script;
    while (count < max) {
        const char *dt = after_tag(p, "<meteogovua-informer-pr-d-dt>");
        if (!dt) {
            break;
        }
        const char *dt_end = strchr(dt, '<');
        const char *title = after_tag(after_tag(dt_end, "<meteogovua-informer-pr-d-icon"), "title=\"");
        const char *title_end = title ? strchr(title, '"') : NULL;
        const char *t = after_tag(title_end, "<meteogovua-informer-pr-d-t>");
        const char *t_end = t ? strchr(t, '<') : NULL;
        if (!dt_end || !title_end || !t_end) {
            break;
        }
        p = t_end;

        forecast_day_t *d = &days[count];
        char buf[64];
        html_decode(dt, (size_t)(dt_end - dt), buf, sizeof(buf));
        if (sscanf(buf, "%d.%d", &d->day, &d->month) != 2) {
            continue;
        }
        html_decode(title, (size_t)(title_end - title), d->text, sizeof(d->text));
        trim(d->text);
        html_decode(t, (size_t)(t_end - t), buf, sizeof(buf));
        char *deg = strstr(buf, "°");
        if (deg) {
            *deg = '\0';
        }
        trim(buf);
        char *end;
        const long v = strtol(buf, &end, 10);
        d->has_temp = end != buf && *end == '\0';
        d->temp = (int)v;
        count++;
    }
    return count;
}

/* Alerts from ubilling.net.ua */

// The keys of the regions in the raw answer of the source "klimenko", in the order of "alerts.regions".
static const char *const REGION_KEYS[] = {
    "м. Київ",
    "Вінницька область",
    "Волинська область",
    "Дніпропетровська область",
    "Донецька область",
    "Житомирська область",
    "Закарпатська область",
    "Запорізька область",
    "Івано-Франківська область",
    "Київська область",
    "Кіровоградська область",
    "Луганська область",
    "Львівська область",
    "Миколаївська область",
    "Одеська область",
    "Полтавська область",
    "Рівненська область",
    "Сумська область",
    "Тернопільська область",
    "Харківська область",
    "Херсонська область",
    "Хмельницька область",
    "Черкаська область",
    "Чернівецька область",
    "Чернігівська область",
    "АР Крим",
    "Севастополь",
};
_Static_assert(sizeof(REGION_KEYS) / sizeof(REGION_KEYS[0]) == T_ALERTS_REGIONS_ITEMS,
               "lang: alerts.regions must have one name per key of REGION_KEYS");

int hadata_region_count(void)
{
    return (int)(sizeof(REGION_KEYS) / sizeof(REGION_KEYS[0]));
}

const char *hadata_region_key(int i)
{
    return i >= 0 && i < hadata_region_count() ? REGION_KEYS[i] : "";
}

const char *hadata_region_name(int i)
{
    return tra(T_ALERTS_REGIONS, i);
}

int hadata_region_find(const char *key)
{
    for (int i = 0; key && key[0] && i < hadata_region_count(); i++) {
        if (strcmp(key, REGION_KEYS[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int hadata_region_find_name(const char *name)
{
    for (int i = 0; name && name[0] && i < hadata_region_count(); i++) {
        if (strcmp(name, hadata_region_name(i)) == 0) {
            return i;
        }
    }
    return -1;
}

// The alerts come from ubilling.net.ua when Home Assistant has no alert entities and a region is set.
static bool ubilling_active(void)
{
    return s_drone.count == 0 && s_missile.count == 0 && s_region[0] != '\0';
}

// Read the alert of the region. The raw answer of the source "klimenko" has the alert levels: "red" (an air raid
// alert) and "yellow" (a threat, for example drones), as the alert entities of Home Assistant. An alert without
// a level counts as red.
static void read_ubilling(time_t now)
{
    char region[sizeof(s_region)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(region, s_region, sizeof(region));
    xSemaphoreGive(s_lock);
    const int status = http_request("https://ubilling.net.ua/aerialalerts/?source=klimenko&raw", NULL, NULL, NULL);
    if (status != 200) {
        ESP_LOGW(TAG, "ubilling.net.ua: HTTP %d", status);
        return;
    }
    cJSON *root = cJSON_Parse(s_body.buf);
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "raw"), region);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(state, "enabled");
    if (!cJSON_IsBool(enabled)) {
        ESP_LOGW(TAG, "ubilling.net.ua: no region \"%s\"", region);
        cJSON_Delete(root);
        return;
    }
    int level = 0;
    time_t since = 0;
    if (cJSON_IsTrue(enabled)) {
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(state, "alert_level");
        level = cJSON_IsString(l) && strcmp(l->valuestring, "yellow") == 0 ? 1 : 2;
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(state, "enabled_at");
        if (!cJSON_IsString(at) || !time_parse_iso8601(at->valuestring, &since)) {
            since = 0;
        }
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(region, s_region) == 0) {
        s_ub_level = level;
        s_ub_since = since;
        s_ub_at = now;
    }
    xSemaphoreGive(s_lock);
}

/* Cities of the forecast */

// The regional centres in the order of "meteo.cities" (Ukrainian alphabet). Donetsk and Luhansk are not in
// the list: meteo.gov.ua does not update their forecasts.
static const char *const CITY_STATIONS[] = { "33562", "34504", "33325", "34602", "33526", "33345", "33711", "33187", "33393", "33846", "33837", "33699", "33506", "33301", "33946", "33275", "33415", "33631", "33587", "34300", "33902", "33429", "33487", "33658", "33135" };
_Static_assert(sizeof(CITY_STATIONS) / sizeof(CITY_STATIONS[0]) == T_METEO_CITIES_ITEMS,
               "lang: meteo.cities must have one name per station of CITY_STATIONS");

// The city and the region of each station for the forecast page of the site (fmi.json), in the order of
// CITY_STATIONS. The site finds the place by these Ukrainian names in each language.
static const char *const CITY_PLACES[][2] = {
    { "Вінниця", "Вінницька" }, { "Дніпро", "Дніпропетровська" }, { "Житомир", "Житомирська" },
    { "Запоріжжя", "Запорізька" }, { "Івано-Франківськ", "Івано-Франківська" }, { "Київ", "Київська" },
    { "Кропивницький", "Кіровоградська" }, { "Луцьк", "Волинська" }, { "Львів", "Львівська" },
    { "Миколаїв", "Миколаївська" }, { "Одеса", "Одеська" }, { "Первомайськ", "Миколаївська" },
    { "Полтава", "Полтавська" }, { "Рівне", "Рівненська" }, { "Сімферополь", "Автономна Республіка Крим" },
    { "Суми", "Сумська" }, { "Тернопіль", "Тернопільська" }, { "Ужгород", "Закарпатська" },
    { "Умань", "Черкаська" }, { "Харків", "Харківська" }, { "Херсон", "Херсонська" },
    { "Хмельницький", "Хмельницька" }, { "Черкаси", "Черкаська" }, { "Чернівці", "Чернівецька" },
    { "Чернігів", "Чернігівська" },
};
_Static_assert(sizeof(CITY_PLACES) / sizeof(CITY_PLACES[0]) == sizeof(CITY_STATIONS) / sizeof(CITY_STATIONS[0]),
               "CITY_PLACES must have one place per station of CITY_STATIONS");

int hadata_city_count(void)
{
    return (int)(sizeof(CITY_STATIONS) / sizeof(CITY_STATIONS[0]));
}

const char *hadata_city_station(int i)
{
    return i >= 0 && i < hadata_city_count() ? CITY_STATIONS[i] : "";
}

const char *hadata_city_name(int i)
{
    return tra(T_METEO_CITIES, i);
}

int hadata_city_find(const char *station)
{
    for (int i = 0; station && station[0] && i < hadata_city_count(); i++) {
        if (strcmp(station, CITY_STATIONS[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int hadata_city_find_name(const char *name)
{
    for (int i = 0; name && name[0] && i < hadata_city_count(); i++) {
        if (strcmp(name, hadata_city_name(i)) == 0) {
            return i;
        }
    }
    return -1;
}

// The current weather of the informer: the temperature (meteogovua-informer-cw-t, "+5&deg;"), the humidity
// (the number after meteogovua-informer-cw-p-d-vp) and the observation time (the title "14:50" of
// meteogovua-informer-header-dt, the local time of today). Return false without a temperature.
static bool parse_current(const char *script, time_t now, int *temp, int *hum, time_t *at)
{
    char buf[32];
    const char *t = after_tag(after_tag(script, "<meteogovua-informer-cw-t"), ">");
    const char *t_end = t ? strchr(t, '<') : NULL;
    if (!t_end) {
        return false;
    }
    html_decode(t, (size_t)(t_end - t), buf, sizeof(buf));
    char *deg = strstr(buf, "°");
    if (deg) {
        *deg = '\0';
    }
    trim(buf);
    char *end;
    const long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || v < -70 || v > 70) {
        return false;
    }
    *temp = (int)v;
    *hum = -1;
    const char *h = after_tag(script, "</meteogovua-informer-cw-p-d-vp>");
    if (h) {
        const long rh = strtol(h, &end, 10);
        if (end != h && rh >= 0 && rh <= 100) {
            *hum = (int)rh;
        }
    }
    *at = now;
    const char *tm_text = after_tag(after_tag(script, "<meteogovua-informer-header-dt"), "title=\"");
    int hh, mm;
    if (tm_text && sscanf(tm_text, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
        struct tm l;
        localtime_r(&now, &l);
        l.tm_hour = hh;
        l.tm_min = mm;
        l.tm_sec = 0;
        time_t obs = mktime(&l);
        if (obs > now + 600) {
            obs -= 24 * 3600;   // the observation of yesterday evening
        }
        *at = obs;
    }
    return true;
}

static void read_forecast(time_t now)
{
    // The informer has the same form in Ukrainian and in English: the forecast text comes in the language
    // of the device.
    char station[sizeof(s_station)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(station, s_station, sizeof(station));
    xSemaphoreGive(s_lock);
    char url[96];
    snprintf(url, sizeof(url), "https://www.meteo.gov.ua/%s/informer/%s.js",
             strcmp(texts_language(), "en") == 0 ? "en" : "ua", station);
    const int status = http_request(url, NULL, NULL, NULL);
    forecast_day_t days[6];
    const int count = status == 200 ? parse_forecast(s_body.buf, days, 6) : 0;
    if (count == 0) {
        ESP_LOGW(TAG, "No forecast from meteo.gov.ua (HTTP %d)", status);
        s_forecast_next = now + FORECAST_RETRY_S;
        return;
    }
    int temp = 0, hum = -1;
    time_t at = 0;
    const bool current = parse_current(s_body.buf, now, &temp, &hum, &at);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_days, days, sizeof(days[0]) * count);
    s_day_count = count;
    s_forecast_at = now;
    s_now_valid = current;
    s_now_temp = temp;
    s_now_hum = hum;
    s_now_at = at;
    xSemaphoreGive(s_lock);
    // The current weather replaces a missing outdoor entity of Home Assistant: then read it more often.
    s_forecast_next = now + (s_temp.count == 0 || s_hum.count == 0 ? WEATHER_REFRESH_S : FORECAST_REFRESH_S);
    ESP_LOGI(TAG, "%d forecast days from meteo.gov.ua, now %s%d°", count, current ? "" : "(none) ", temp);
}

/* The city forecast of meteo.gov.ua */

static esp_err_t on_days_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        fmi_parser_feed(&s_fmi_parser, e->data, (size_t)e->data_len);
    }
    return ESP_OK;
}

// Percent-encode a UTF-8 text for a URL query.
static void url_encode(char *out, size_t cap, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    out[o] = '\0';
}

// Return true if the city forecast must be read now.
static bool days_due(time_t now)
{
    if (now < s_fmi_next) {
        return false;
    }
    if (s_fmi_at == 0) {
        return true;
    }
    // The last reading must be after the last 06:00 or 18:00.
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_hour >= DAYS_EVENING_HOUR) {
        t.tm_hour = DAYS_EVENING_HOUR;
    } else if (t.tm_hour >= DAYS_MORNING_HOUR) {
        t.tm_hour = DAYS_MORNING_HOUR;
    } else {
        t.tm_mday--;
        t.tm_hour = DAYS_EVENING_HOUR;
    }
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    return s_fmi_at < mktime(&t);
}

// Read the forecast of the city from the forecast page of the site. The answer streams through the parser.
static void read_days(time_t now)
{
    char station[sizeof(s_station)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(station, s_station, sizeof(station));
    xSemaphoreGive(s_lock);
    const int i = hadata_city_find(station);
    if (i < 0) {
        s_fmi_next = now + DAYS_RETRY_S;
        s_days_reads++;
        return;
    }
    char city[96], region[192], url[DAYS_URL_LEN];
    url_encode(city, sizeof(city), CITY_PLACES[i][0]);
    url_encode(region, sizeof(region), CITY_PLACES[i][1]);
    snprintf(url, sizeof(url), "https://www.meteo.gov.ua/fmi.json?city=%s&region=%s&lang=ua&action=getCityWeather",
             city, region);
    struct tm t;
    localtime_r(&now, &t);
    fmi_parser_init(&s_fmi_parser, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .event_handler = on_days_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        s_fmi_next = now + DAYS_RETRY_S;
        s_days_reads++;
        return;
    }
    const esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    const int count = status == 200 ? fmi_parser_count(&s_fmi_parser) : 0;
    s_days_reads++;
    if (count == 0) {
        ESP_LOGW(TAG, "No city forecast from meteo.gov.ua (HTTP %d, %s)", status, esp_err_to_name(err));
        s_fmi_next = now + DAYS_RETRY_S;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(station, s_station) == 0) {
        memcpy(s_fmi_days, s_fmi_parser.days, sizeof(s_fmi_days));
        s_fmi_at = now;
    }
    xSemaphoreGive(s_lock);
    s_fmi_next = 0;
    ESP_LOGI(TAG, "City forecast from meteo.gov.ua: %d days", count);
}

/* healthchecks.io */

// A UUID (36 characters) or a unique_key (40 hex characters) identifies a check directly.
// Other text is a slug.
static bool is_check_id(const char *s)
{
    const size_t n = strlen(s);
    if (n != 36 && n != 40) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i]) && s[i] != '-') {
            return false;
        }
    }
    return true;
}

// Read the status of the check. "up" means that the grid has power. "grace" (the ping is late) and "down"
// mean no power: the pinging device is on the grid only, so a late ping is the first sign of an outage.
// "new" and "paused" tell nothing.
static void read_healthcheck(time_t now)
{
    char url[160];
    const bool by_id = is_check_id(s_hc_check);
    snprintf(url, sizeof(url), by_id ? "https://healthchecks.io/api/v3/checks/%s"
                                     : "https://healthchecks.io/api/v3/checks/?slug=%s", s_hc_check);
    const int status = http_request(url, NULL, "X-Api-Key", s_hc_key);
    if (status != 200) {
        ESP_LOGW(TAG, "healthchecks.io: HTTP %d", status);
        return;
    }
    cJSON *root = cJSON_Parse(s_body.buf);
    const cJSON *check = by_id ? root : cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "checks"), 0);
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(check, "status");
    if (cJSON_IsString(st)) {
        int dtek = -1;
        if (strcmp(st->valuestring, "up") == 0) {
            dtek = 1;
        } else if (strcmp(st->valuestring, "down") == 0 || strcmp(st->valuestring, "grace") == 0) {
            dtek = 0;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_hc_dtek = dtek;
        s_hc_at = now;
        strlcpy(s_hc_state, st->valuestring, sizeof(s_hc_state));
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGW(TAG, "healthchecks.io: no check \"%s\"", s_hc_check);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_hc_state, tr(T_DATA_HC_NONE), sizeof(s_hc_state));
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(root);
}

/* DTEK state */

// The DTEK state from the first source that knows it: the Home Assistant entity, the healthchecks.io
// check, the ping. The caller holds s_lock. Return the source; *dtek is -1 when no source knows.
static int dtek_state(time_t now, int *dtek)
{
    if (s_data.updated && now - s_data.updated <= s_ha_max_age && s_data.dtek >= 0) {
        *dtek = s_data.dtek;
        return HADATA_DTEK_HA;
    }
    if (s_hc_key[0] && s_hc_at && now - s_hc_at <= s_hc_max_age && s_hc_dtek >= 0) {
        *dtek = s_hc_dtek;
        return HADATA_DTEK_HC;
    }
    if (s_ping_host[0] && s_ping_at && now - s_ping_at <= s_hc_max_age && s_ping_dtek >= 0) {
        *dtek = s_ping_dtek;
        return HADATA_DTEK_PING;
    }
    *dtek = -1;
    return HADATA_DTEK_NONE;
}

// True when the Home Assistant entity or the healthchecks.io check knows the DTEK state.
static bool dtek_known_without_ping(time_t now)
{
    int dtek;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int source = dtek_state(now, &dtek);
    xSemaphoreGive(s_lock);
    return source == HADATA_DTEK_HA || source == HADATA_DTEK_HC;
}

/* Ping */

static void ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    xSemaphoreGive((SemaphoreHandle_t)args);
}

// Ping the device without backup power: it answers only while DTEK gives power.
// Return 1 when it answers, else 0. No address for the name also means no device: no DTEK power.
static int ping_device(void)
{
    const struct addrinfo hints = { .ai_family = AF_INET };
    struct addrinfo *res = NULL;
    if (getaddrinfo(s_ping_host, NULL, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "Ping: no address for %s", s_ping_host);
        return 0;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    const struct in_addr addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target), &addr);
    freeaddrinfo(res);
    cfg.target_addr = target;
    cfg.count = PING_COUNT;
    cfg.interval_ms = PING_INTERVAL_MS;
    cfg.timeout_ms = PING_TIMEOUT_MS;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        return 0;
    }
    const esp_ping_callbacks_t cbs = { .cb_args = done, .on_ping_end = ping_end };
    esp_ping_handle_t ping = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        ESP_LOGW(TAG, "Ping: cannot start");
        vSemaphoreDelete(done);
        return 0;
    }
    esp_ping_start(ping);
    xSemaphoreTake(done, pdMS_TO_TICKS(PING_COUNT * (PING_INTERVAL_MS + PING_TIMEOUT_MS) + 2000));
    uint32_t replies = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &replies, sizeof(replies));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(done);
    return replies > 0 ? 1 : 0;
}

/* Time zone */

// The time zone of Home Assistant: the offset now, then the first change in the next 400 days. The template
// scans the days, then the hours of that day, in UTC (as_local gives the offset of each moment). Home Assistant
// has the tz database: a new law comes with its update.
static const char TZ_TEMPLATE[] =
    "{%- set t0 = utcnow().replace(minute=0, second=0, microsecond=0) -%}"
    "{%- set off0 = (t0 | as_local).utcoffset().total_seconds() | int -%}"
    "{%- set ns = namespace(day=0, at=0, off=off0) -%}"
    "{%- for d in range(1, 400) -%}"
    "{%- if ns.day == 0 and ((t0 + timedelta(days=d)) | as_local).utcoffset().total_seconds() | int != off0 -%}"
    "{%- set ns.day = d -%}{%- endif -%}{%- endfor -%}"
    "{%- if ns.day > 0 -%}{%- for h in range(0, 25) -%}"
    "{%- set t = t0 + timedelta(days=ns.day - 1, hours=h) -%}"
    "{%- set o = (t | as_local).utcoffset().total_seconds() | int -%}"
    "{%- if ns.at == 0 and o != off0 -%}{%- set ns.at = as_timestamp(t) | int -%}{%- set ns.off = o -%}"
    "{%- endif -%}{%- endfor -%}{%- endif -%}"
    "{{ {'offset': off0, 'next': ns.at, 'next_offset': ns.off} | tojson }}";

#define TZ_MAX_OFFSET (14 * 3600)

static volatile bool s_tz_request;
static hadata_tz_t s_tz;        // under s_lock
static unsigned s_tz_count;     // under s_lock

void hadata_request_tz(void)
{
    s_tz_request = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

unsigned hadata_get_tz(hadata_tz_t *out)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_tz;
    const unsigned count = s_tz_count;
    xSemaphoreGive(s_lock);
    return count;
}

static bool tz_number(const cJSON *root, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static void read_tz(void)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "template", TZ_TEMPLATE);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return;
    }
    char url[160];
    snprintf(url, sizeof(url), "%s/api/template", s_url);
    const int status = http_request(url, body, "Authorization", s_bearer);
    cJSON_free(body);
    if (status != 200) {
        ESP_LOGW(TAG, "Time zone: HTTP %d %.120s", status, s_body.buf);
        return;
    }
    cJSON *root = cJSON_Parse(s_body.buf);
    double offset, next, next_offset;
    if (!tz_number(root, "offset", &offset) || !tz_number(root, "next", &next) ||
        !tz_number(root, "next_offset", &next_offset) || offset < -TZ_MAX_OFFSET || offset > TZ_MAX_OFFSET ||
        next_offset < -TZ_MAX_OFFSET || next_offset > TZ_MAX_OFFSET || next < 0) {
        ESP_LOGW(TAG, "Time zone: unexpected answer %.120s", s_body.buf);
        cJSON_Delete(root);
        return;
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tz.offset = (int)offset;
    s_tz.next = (time_t)next;
    s_tz.next_offset = (int)next_offset;
    s_tz_count++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Time zone: offset %d s, next change %lld to %d s", (int)offset, (long long)next,
             (int)next_offset);
}

/* Task */

static volatile bool s_paused;

bool hadata_days_due(void)
{
    return s_station[0] && hadata_city_find(s_station) >= 0 && days_due(time(NULL));
}

unsigned hadata_days_reads(void)
{
    return s_days_reads;
}

void hadata_pause(bool paused)
{
    if (s_paused == paused) {
        return;
    }
    s_paused = paused;
    ESP_LOGI(TAG, "Requests %s", paused ? "stop (the screen comes from trmnl-server)" : "start again");
    // Read now after the pause.
    if (!paused && s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void hadata_task(void *arg)
{
    (void)arg;
    const bool configured = s_url[0] && s_token[0] && s_template;
    const bool hc_configured = s_hc_key[0] && s_hc_check[0];
    time_t hc_next = 0;
    for (;;) {
        const time_t now = time(NULL);
        // The time zone: one request when the main task asks, also while the other requests stop.
        if (s_tz_request && configured && net_connected() && now >= VALID_TIME) {
            s_tz_request = false;
            read_tz();
            if (s_on_change) {
                s_on_change();
            }
        }
        const bool connected = net_connected() && now >= VALID_TIME;
        const bool online = !s_paused && connected;
        if (s_paused && connected && s_station[0] && days_due(now)) {
            // The server screen stops the other requests. The city forecast still comes twice a day: then
            // the forecast screens have it at once after the switch.
            read_days(now);
            if (s_on_change) {
                s_on_change();
            }
        }
        if (online) {
            if (configured) {
                read_states(now);
                read_one_dayago(now);
            }
            if (hc_configured && now >= hc_next) {
                read_healthcheck(now);
                hc_next = now + HC_PERIOD_S;
            }
            if (ubilling_active()) {
                read_ubilling(now);
            }
            if (s_station[0] && now >= s_forecast_next) {
                read_forecast(now);
            }
            if (s_station[0] && days_due(now)) {
                read_days(now);
            }
            // The ping is the last source: only when the entity and the check tell nothing.
            if (s_ping_host[0] && !dtek_known_without_ping(now)) {
                const int answer = ping_device();
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_ping_dtek = answer;
                s_ping_at = now;
                xSemaphoreGive(s_lock);
            }
            s_reads++;
            if (s_on_change) {
                s_on_change();
            }
        }
        // hadata_refresh() and hadata_pause(false) can wake the task before the time.
        ulTaskNotifyTake(pdTRUE, s_paused ? portMAX_DELAY
                                          : pdMS_TO_TICKS(online ? READ_PERIOD_MS : OFFLINE_PERIOD_MS));
    }
}

void hadata_start(const settings_t *settings, hadata_change_cb_t on_change)
{
    s_on_change = on_change;
    s_lock = xSemaphoreCreateMutex();

    // Keep the cJSON trees in PSRAM: a history answer can have some hundred entries.
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    strlcpy(s_url, settings->ha_url, sizeof(s_url));
    for (int n = (int)strlen(s_url); n > 0 && s_url[n - 1] == '/'; n--) {
        s_url[n - 1] = '\0';
    }
    strlcpy(s_token, settings->ha_token, sizeof(s_token));
    snprintf(s_bearer, sizeof(s_bearer), "Bearer %s", s_token);
    strlcpy(s_station, settings->meteo, sizeof(s_station));
    strlcpy(s_region, settings->alert_reg, sizeof(s_region));
    if (s_station[0] && hadata_city_find(s_station) < 0) {
        ESP_LOGW(TAG, "Forecast: station %s is not in the list of cities", s_station);
    }
    // The key and the check go into a URL and a header: keep only safe characters.
    size_t n = 0;
    for (const char *p = settings->hc_check; *p && n + 1 < sizeof(s_hc_check); p++) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') {
            s_hc_check[n++] = *p;
        }
    }
    s_hc_check[n] = '\0';
    n = 0;
    for (const char *p = settings->hc_key; *p && n + 1 < sizeof(s_hc_key); p++) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') {
            s_hc_key[n++] = *p;
        }
    }
    s_hc_key[n] = '\0';
    // A host name or an IPv4 address.
    n = 0;
    for (const char *p = settings->ping_host; *p && n + 1 < sizeof(s_ping_host); p++) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '.') {
            s_ping_host[n++] = *p;
        }
    }
    s_ping_host[n] = '\0';
    parse_list(settings->e_temp, &s_temp);
    parse_list(settings->e_hum, &s_hum);
    parse_list(settings->e_aqi, &s_aqi);
    parse_list(settings->e_volt, &s_volt);
    parse_list(settings->e_ups, &s_ups);
    parse_list(settings->e_drone, &s_drone);
    parse_list(settings->e_missile, &s_missile);
    parse_list(settings->e_off, &s_off);
    parse_list(settings->e_on, &s_on);
    parse_list(settings->e_dtek, &s_dtek);

    const entity_list_t *with_history[] = { &s_temp, &s_hum, &s_aqi };
    for (int l = 0; l < 3; l++) {
        for (int i = 0; i < with_history[l]->count && s_dayago_count < 3 * MAX_LIST; i++) {
            strlcpy(s_dayago[s_dayago_count++].entity, with_history[l]->id[i], ENTITY_LEN);
        }
    }

    s_body.cap = MAX_BODY;
    s_body.buf = heap_caps_malloc(MAX_BODY, MALLOC_CAP_SPIRAM);
    if (!s_body.buf) {
        ESP_LOGE(TAG, "No memory for the answers");
        return;
    }
    s_body.buf[0] = '\0';

    s_data.configured = s_url[0] && s_token[0];
    if (!s_data.configured) {
        strlcpy(s_data.error, tr(T_DATA_NOT_CONFIGURED), sizeof(s_data.error));
        ESP_LOGW(TAG, "%s", s_data.error);
    } else {
        s_template = make_template();
    }
    xTaskCreate(hadata_task, "hadata", 10240, NULL, 4, &s_task);
}

void hadata_refresh(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void hadata_set_region(const char *key)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_region, key ? key : "", sizeof(s_region));
    s_ub_level = -1;            // the alert of the old region is not shown
    s_ub_since = 0;
    s_ub_at = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Alerts: region \"%s\"", s_region);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void hadata_set_station(const char *station)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_station, station ? station : "", sizeof(s_station));
    s_day_count = 0;            // the forecast of the old city is not shown
    s_now_valid = false;
    s_forecast_next = 0;        // read the new one at the next chance
    s_fmi_at = 0;
    s_fmi_next = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Forecast: station \"%s\"", s_station);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

unsigned hadata_read_count(void)
{
    return s_reads;
}

void hadata_set_max_age(int ha_seconds, int other_seconds)
{
    s_ha_max_age = ha_seconds > 0 ? ha_seconds : HA_MAX_AGE_S;
    s_hc_max_age = other_seconds > HC_MAX_AGE_S ? other_seconds : HC_MAX_AGE_S;
}

void hadata_get(ha_data_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    const time_t now = time(NULL);
    strlcpy(out->hc_state, s_hc_key[0] ? s_hc_state : "", sizeof(out->hc_state));
    out->hc_at = s_hc_key[0] ? s_hc_at : 0;
    out->ping_dtek = s_ping_host[0] ? s_ping_dtek : -1;
    out->ping_at = s_ping_host[0] ? s_ping_at : 0;
    out->dtek_source = dtek_state(now, &out->dtek);
    // Without the alert entities of Home Assistant: the alert of ubilling.net.ua, if it is not too old.
    out->alert_ubilling = ubilling_active();
    out->alert_at = out->alert_ubilling ? s_ub_at : 0;
    if (out->alert_ubilling) {
        const bool fresh = s_ub_at && s_ub_level >= 0 && now - s_ub_at <= s_ha_max_age;
        out->alert_known = fresh;
        out->drone = fresh && s_ub_level == 1;
        out->missile = fresh && s_ub_level == 2;
        out->alert_since = fresh && s_ub_level > 0 ? s_ub_since : 0;
    }
    // Without an outdoor temperature or humidity of Home Assistant (no entity, or it does not answer):
    // the current weather of meteo.gov.ua, if the observation is not too old. It has no change in 24 hours.
    out->outdoor_meteo = false;
    out->outdoor_at = 0;
    if (s_station[0] && s_now_valid && now - s_now_at <= WEATHER_MAX_AGE_S) {
        if (!out->temperature.valid) {
            out->temperature = (ha_sensor_t){ .valid = true, .value = s_now_temp };
            out->outdoor_meteo = true;
        }
        if (!out->humidity.valid && s_now_hum >= 0) {
            out->humidity = (ha_sensor_t){ .valid = true, .value = s_now_hum };
            out->outdoor_meteo = true;
        }
        out->outdoor_at = out->outdoor_meteo ? s_now_at : 0;
    }
    // The forecast for tomorrow, if it is not too old.
    out->forecast_valid = false;
    if (s_day_count > 0 && now - s_forecast_at <= FORECAST_MAX_AGE_S) {
        const time_t tomorrow = now + 24 * 3600;
        struct tm t;
        localtime_r(&tomorrow, &t);
        for (int i = 0; i < s_day_count; i++) {
            if (s_days[i].day == t.tm_mday && s_days[i].month == t.tm_mon + 1) {
                out->forecast_valid = true;
                out->forecast_has_temp = s_days[i].has_temp;
                out->forecast_temp = s_days[i].temp;
                strlcpy(out->forecast_text, s_days[i].text, sizeof(out->forecast_text));
                break;
            }
        }
    }
    // The city forecast by dates: days[0] is today. After midnight the dates move without a new reading.
    memset(out->days, 0, sizeof(out->days));
    out->days_at = 0;
    if (s_fmi_at && now - s_fmi_at <= DAYS_MAX_AGE_S) {
        struct tm t;
        localtime_r(&now, &t);
        for (int i = 0; i < HADATA_DAYS; i++) {
            struct tm d = t;
            d.tm_mday += i;
            d.tm_hour = 12;
            d.tm_isdst = -1;
            mktime(&d);
            for (int k = 0; k < FMI_DAYS; k++) {
                const fmi_day_t *f = &s_fmi_days[k];
                if (f->year == d.tm_year + 1900 && f->month == d.tm_mon + 1 && f->mday == d.tm_mday) {
                    out->days[i].night = (hadata_point_t){ f->night.valid, f->night.temp, f->night.symbol };
                    out->days[i].day = (hadata_point_t){ f->day.valid, f->day.temp, f->day.symbol };
                    break;
                }
            }
        }
        out->days_at = s_fmi_at;
    }
    // Without Home Assistant the screens take the data as fresh when a source has fresh data: each source
    // checks the age of its own data above.
    if (!out->configured && (out->outdoor_meteo || out->forecast_valid || out->alert_known || out->days_at)) {
        out->updated = now;
    }
    xSemaphoreGive(s_lock);
}
