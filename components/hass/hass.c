// MQTT client for Home Assistant.
// Topics: rlcd/<id>/status ("online"/"offline", retained), rlcd/<id>/state (JSON),
// rlcd/<id>/<control> (control state, retained) and rlcd/<id>/<control>/set (commands; Home Assistant
// keeps them retained), <prefix>/<component>/<id>/<key>/config (discovery, retained).
#include "hass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"
#include "hadata.h"
#include "net.h"
#include "texts.h"

static const char *TAG = "hass";

// The time is not known before this moment (2024-01-01 00:00 UTC).
#define VALID_TIME 1704067200

static esp_mqtt_client_handle_t s_client;
static hass_change_cb_t s_on_change;
static hass_command_cb_t s_on_command;
static volatile bool s_connected;
// The start of the MQTT session (UTC), 0 = not known. In eco mode and on the server screen each session is
// a new connection, so the "session" sensor shows each one in the activity of Home Assistant.
static time_t s_connected_at;
static bool s_running;
static char s_id[16];
static char s_avail_topic[48];
static char s_state_topic[48];
static char s_prefix[32];
static char s_version[48];
static char s_uri[128];
static char s_user[64];
static char s_pass[64];
static char s_error[64];
static const settings_t *s_settings;    // the screens that are available (settings_screen_available())

// The screen ids in the order of the list screen.names (ui_screen_t).
static const char *const SCREEN_IDS[] = { "dashboard", "room", "note", "quad", "quad5", "days10", "server",
                                          "clock" };
_Static_assert(sizeof(SCREEN_IDS) / sizeof(SCREEN_IDS[0]) == T_SCREEN_NAMES_ITEMS, "lang: screen.names");

typedef struct {
    const char *component;  // Home Assistant platform
    const char *key;        // field in the state JSON (sensors) or part of the topic (controls)
    text_id_t name;         // entity name, shown in Home Assistant
    const char *extra;      // more discovery fields
} entity_t;

static const entity_t SENSORS[] = {
    { "sensor", "temperature", T_HA_TEMPERATURE,
      "\"device_class\":\"temperature\",\"unit_of_measurement\":\"°C\",\"state_class\":\"measurement\","
      "\"suggested_display_precision\":1" },
    { "sensor", "humidity", T_HA_HUMIDITY,
      "\"device_class\":\"humidity\",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\","
      "\"suggested_display_precision\":0" },
    { "sensor", "battery", T_HA_BATTERY,
      "\"device_class\":\"battery\",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\"" },
    { "sensor", "voltage", T_HA_VOLTAGE,
      "\"device_class\":\"voltage\",\"unit_of_measurement\":\"V\",\"state_class\":\"measurement\","
      "\"suggested_display_precision\":2,\"entity_category\":\"diagnostic\"" },
    { "sensor", "rssi", T_HA_RSSI,
      "\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\","
      "\"entity_category\":\"diagnostic\"" },
    { "binary_sensor", "usb", T_HA_USB,
      "\"device_class\":\"plug\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" },
    { "sensor", "session", T_HA_SESSION,
      "\"device_class\":\"timestamp\",\"icon\":\"mdi:sync\"" },
};

// Controls: Home Assistant sends commands to rlcd/<id>/<key>/set. "retain": the broker keeps
// the last command, so a sleeping device gets it at the next update.
static const entity_t CONTROLS[] = {
    { "switch", "eco", T_HA_ECO,
      "\"icon\":\"mdi:leaf\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" },
    { "number", "eco_min", T_HA_ECO_MIN,
      "\"icon\":\"mdi:timer-outline\",\"min\":5,\"max\":240,\"step\":5,\"unit_of_measurement\":\"min\","
      "\"mode\":\"box\",\"entity_category\":\"config\"" },
    { "switch", "alert_sound", T_HA_ALERT_SOUND,
      "\"icon\":\"mdi:alarm-light-outline\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" },
    { "number", "volume", T_HA_VOLUME,
      "\"icon\":\"mdi:volume-high\",\"min\":0,\"max\":100,\"step\":5,\"unit_of_measurement\":\"%\","
      "\"mode\":\"slider\",\"entity_category\":\"config\"" },
    // The options of a select come from select_options(): the names of the current language.
    { "select", "screen", T_HA_SCREEN, "\"icon\":\"mdi:monitor-dashboard\"" },
    { "number", "srv_min", T_HA_SRV_MIN,
      "\"icon\":\"mdi:timer-sync-outline\",\"min\":1,\"max\":30,\"step\":1,\"unit_of_measurement\":\"min\","
      "\"mode\":\"box\",\"entity_category\":\"config\"" },
    { "select", "meteo", T_HA_METEO, "\"icon\":\"mdi:weather-partly-cloudy\",\"entity_category\":\"config\"" },
    { "select", "alert_reg", T_HA_ALERT_REG, "\"icon\":\"mdi:alarm-light\",\"entity_category\":\"config\"" },
};

#define CONTROL_COUNT (sizeof(CONTROLS) / sizeof(CONTROLS[0]))

// The values have room for the screen and city names (UTF-8).
#define VALUE_MAX 64
static char s_control_value[CONTROL_COUNT][VALUE_MAX];   // the states to publish
static char s_pending_command[CONTROL_COUNT][VALUE_MAX]; // local changes for the command topics, "" = none
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

static void notify(void)
{
    if (s_on_change) {
        s_on_change();
    }
}

static void control_topic(char *buf, size_t len, const char *key, bool command)
{
    snprintf(buf, len, "rlcd/%s/%s%s", s_id, key, command ? "/set" : "");
}

// The common end of a discovery payload: availability and the device.
static int device_json(char *buf, size_t len)
{
    char ip[16];
    net_ip(ip, sizeof(ip));
    return snprintf(buf, len,
                    "\"availability_topic\":\"%s\","
                    "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"RLCD Dashboard\",\"manufacturer\":\"Waveshare\","
                    "\"model\":\"ESP32-S3-RLCD-4.2\",\"sw_version\":\"%s\"%s%s%s}}",
                    s_avail_topic, s_id, s_version,
                    ip[0] ? ",\"configuration_url\":\"http://" : "", ip, ip[0] ? "/\"" : "");
}

// The options of a select as JSON: ,"options":["...","..."]. The names are in the current language.
static void select_options(const char *key, char *buf, size_t len)
{
    size_t n = (size_t)snprintf(buf, len, ",\"options\":[");
    if (strcmp(key, "screen") == 0) {
        bool first = true;
        for (int i = 0; i < T_SCREEN_NAMES_ITEMS && n < len; i++) {
            if (s_settings && !settings_screen_available(s_settings, SCREEN_IDS[i])) {
                continue;
            }
            n += (size_t)snprintf(buf + n, len - n, "%s\"%s\"", first ? "" : ",", tra(T_SCREEN_NAMES, i));
            first = false;
        }
    } else if (strcmp(key, "meteo") == 0) {
        n += (size_t)snprintf(buf + n, len - n, "\"%s\"", tr(T_METEO_NONE));
        for (int i = 0; i < hadata_city_count() && n < len; i++) {
            n += (size_t)snprintf(buf + n, len - n, ",\"%s\"", hadata_city_name(i));
        }
    } else if (strcmp(key, "alert_reg") == 0) {
        n += (size_t)snprintf(buf + n, len - n, "\"%s\"", tr(T_ALERTS_NONE));
        for (int i = 0; i < hadata_region_count() && n < len; i++) {
            n += (size_t)snprintf(buf + n, len - n, ",\"%s\"", hadata_region_name(i));
        }
    }
    if (n < len) {
        snprintf(buf + n, len - n, "]");
    }
}

#define PAYLOAD_MAX 3072
#define OPTIONS_MAX 2048

static void publish_discovery(void)
{
    char *payload = malloc(PAYLOAD_MAX);
    char *options = malloc(OPTIONS_MAX);
    if (!payload || !options) {
        free(payload);
        free(options);
        return;
    }
    char topic[160];
    char tmpl[96];
    for (size_t i = 0; i < sizeof(SENSORS) / sizeof(SENSORS[0]); i++) {
        const entity_t *e = &SENSORS[i];
        snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", s_prefix, e->component, s_id, e->key);
        if (strcmp(e->component, "binary_sensor") == 0) {
            snprintf(tmpl, sizeof(tmpl), "{{ 'ON' if value_json.%s else 'OFF' }}", e->key);
        } else {
            snprintf(tmpl, sizeof(tmpl), "{{ value_json.%s }}", e->key);
        }
        const int n = snprintf(payload, PAYLOAD_MAX,
                               "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s\","
                               "\"value_template\":\"%s\",%s,",
                               tr(e->name), s_id, e->key, s_state_topic, tmpl, e->extra);
        if (n > 0 && n < PAYLOAD_MAX) {
            device_json(payload + n, PAYLOAD_MAX - n);
        }
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
    }

    char state[64], command[64];
    for (size_t i = 0; i < CONTROL_COUNT; i++) {
        const entity_t *e = &CONTROLS[i];
        snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", s_prefix, e->component, s_id, e->key);
        control_topic(state, sizeof(state), e->key, false);
        control_topic(command, sizeof(command), e->key, true);
        options[0] = '\0';
        if (strcmp(e->component, "select") == 0) {
            select_options(e->key, options, OPTIONS_MAX);
        }
        const int n = snprintf(payload, PAYLOAD_MAX,
                               "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"command_topic\":\"%s\","
                               "\"state_topic\":\"%s\",\"retain\":true,%s%s,",
                               tr(e->name), s_id, e->key, command, state, e->extra, options);
        if (n > 0 && n < PAYLOAD_MAX) {
            device_json(payload + n, PAYLOAD_MAX - n);
        }
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
    }
    free(payload);
    free(options);
    ESP_LOGI(TAG, "Discovery is published for %s", s_id);
}

static void publish_control_states(void)
{
    char topic[64];
    for (size_t i = 0; i < CONTROL_COUNT; i++) {
        if (s_control_value[i][0]) {
            control_topic(topic, sizeof(topic), CONTROLS[i].key, false);
            esp_mqtt_client_publish(s_client, topic, s_control_value[i], 0, 1, 1);
        }
    }
}

// Publish the local changes to the command topics, retained. Then the retained command of Home Assistant
// is the same as the device value, and it does not change the value back after the next connection.
static void publish_pending_commands(void)
{
    char topic[64];
    for (size_t i = 0; i < CONTROL_COUNT; i++) {
        char value[sizeof(s_pending_command[i])];
        taskENTER_CRITICAL(&s_pending_lock);
        strlcpy(value, s_pending_command[i], sizeof(value));
        s_pending_command[i][0] = '\0';
        taskEXIT_CRITICAL(&s_pending_lock);
        if (value[0]) {
            control_topic(topic, sizeof(topic), CONTROLS[i].key, true);
            esp_mqtt_client_publish(s_client, topic, value, 0, 1, 1);
        }
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t e = data;
    char topic[64];
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to %s", s_uri);
        const time_t now = time(NULL);
        s_connected_at = now >= VALID_TIME ? now : 0;
        s_connected = true;
        s_error[0] = '\0';
        esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, 1);
        publish_discovery();
        publish_control_states();
        // Local changes first: the broker handles the messages in order, so the subscription below
        // gets the new retained values.
        publish_pending_commands();
        // The broker sends the retained commands at once.
        for (size_t i = 0; i < CONTROL_COUNT; i++) {
            control_topic(topic, sizeof(topic), CONTROLS[i].key, true);
            esp_mqtt_client_subscribe(s_client, topic, 1);
        }
        notify();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGI(TAG, "Disconnected from %s", s_uri);
        }
        s_connected = false;
        notify();
        break;
    case MQTT_EVENT_DATA:
        if (e && s_on_command && e->data_len > 0 && e->data_len < VALUE_MAX) {
            char value[VALUE_MAX];
            memcpy(value, e->data, e->data_len);
            value[e->data_len] = '\0';
            for (size_t i = 0; i < CONTROL_COUNT; i++) {
                control_topic(topic, sizeof(topic), CONTROLS[i].key, true);
                if (e->topic_len == (int)strlen(topic) && strncmp(e->topic, topic, e->topic_len) == 0) {
                    s_on_command(CONTROLS[i].key, value);
                    break;
                }
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        if (e && e->error_handle) {
            if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                snprintf(s_error, sizeof(s_error), tr(T_HA_BROKER_REFUSED),
                         (int)e->error_handle->connect_return_code);
            } else if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                strlcpy(s_error, tr(T_HA_NO_BROKER), sizeof(s_error));
            }
            ESP_LOGW(TAG, "MQTT error: %s", s_error);
        }
        break;
    default:
        break;
    }
}

esp_err_t hass_start(const settings_t *settings, const char *version, hass_change_cb_t on_change,
                     hass_command_cb_t on_command)
{
    s_on_change = on_change;
    s_on_command = on_command;
    if (settings->mqtt_uri[0] == '\0') {
        ESP_LOGI(TAG, "No MQTT broker in the settings: MQTT is off");
        return ESP_OK;
    }
    strlcpy(s_uri, settings->mqtt_uri, sizeof(s_uri));
    strlcpy(s_user, settings->mqtt_user, sizeof(s_user));
    strlcpy(s_pass, settings->mqtt_pass, sizeof(s_pass));
    s_settings = settings;
    strlcpy(s_prefix, settings->mqtt_prefix[0] ? settings->mqtt_prefix : "homeassistant", sizeof(s_prefix));
    strlcpy(s_version, version, sizeof(s_version));

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "rlcd_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_avail_topic, sizeof(s_avail_topic), "rlcd/%s/status", s_id);
    snprintf(s_state_topic, sizeof(s_state_topic), "rlcd/%s/state", s_id);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.client_id = s_id,
        .credentials.username = s_user[0] ? s_user : NULL,
        .credentials.authentication.password = s_pass[0] ? s_pass : NULL,
        .session.keepalive = 60,
        .session.last_will.topic = s_avail_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = 10000,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Cannot make the MQTT client");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    ESP_LOGI(TAG, "Connect to %s as %s", s_uri, s_id);
    const esp_err_t err = esp_mqtt_client_start(s_client);
    s_running = err == ESP_OK;
    return err;
}

bool hass_enabled(void)
{
    return s_client != NULL;
}

bool hass_connected(void)
{
    return s_connected;
}

const char *hass_last_error(void)
{
    return s_error;
}

void hass_set_state(const char *key, const char *value)
{
    for (size_t i = 0; i < CONTROL_COUNT; i++) {
        if (strcmp(CONTROLS[i].key, key) == 0) {
            strlcpy(s_control_value[i], value, sizeof(s_control_value[i]));
            if (s_connected) {
                char topic[64];
                control_topic(topic, sizeof(topic), key, false);
                esp_mqtt_client_publish(s_client, topic, value, 0, 1, 1);
            }
            return;
        }
    }
}

void hass_set_command(const char *key, const char *value)
{
    if (!s_client) {
        return;
    }
    for (size_t i = 0; i < CONTROL_COUNT; i++) {
        if (strcmp(CONTROLS[i].key, key) == 0) {
            taskENTER_CRITICAL(&s_pending_lock);
            strlcpy(s_pending_command[i], value, sizeof(s_pending_command[i]));
            taskEXIT_CRITICAL(&s_pending_lock);
            if (s_connected) {
                publish_pending_commands();
            }
            return;
        }
    }
}

void hass_update_discovery(void)
{
    if (s_client && s_connected) {
        publish_discovery();
    }
}

void hass_suspend(void)
{
    if (!s_client || !s_running) {
        return;
    }
    // DISCONNECT first: then the broker does not send the last will ("offline").
    if (s_connected) {
        esp_mqtt_client_disconnect(s_client);
    }
    esp_mqtt_client_stop(s_client);
    s_running = false;
    s_connected = false;
}

void hass_resume(void)
{
    if (!s_client || s_running) {
        return;
    }
    s_running = esp_mqtt_client_start(s_client) == ESP_OK;
}

void hass_publish(const hass_state_t *st)
{
    if (!s_connected) {
        return;
    }
    char t[16] = "null", h[16] = "null", b[16] = "null", v[16] = "null";
    if (st->room_valid) {
        snprintf(t, sizeof(t), "%.1f", (double)st->room_temperature);
        snprintf(h, sizeof(h), "%.0f", (double)st->room_humidity);
    }
    if (st->battery_percent >= 0) {
        snprintf(b, sizeof(b), "%d", st->battery_percent);
    }
    if (st->battery_mv > 0) {
        snprintf(v, sizeof(v), "%.2f", st->battery_mv / 1000.0);
    }
    // The start of the session as an ISO 8601 time in UTC (Home Assistant shows it in the local time).
    char session[40] = "null";
    if (s_connected_at) {
        struct tm utc;
        gmtime_r(&s_connected_at, &utc);
        strftime(session, sizeof(session), "\"%Y-%m-%dT%H:%M:%S+00:00\"", &utc);
    }
    char json[240];
    snprintf(json, sizeof(json),
             "{\"temperature\":%s,\"humidity\":%s,\"battery\":%s,\"voltage\":%s,\"rssi\":%d,\"usb\":%s,"
             "\"session\":%s}",
             t, h, b, v, st->rssi, st->usb_power ? "true" : "false", session);
    esp_mqtt_client_publish(s_client, s_state_topic, json, 0, 0, 0);
}
