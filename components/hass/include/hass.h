// MQTT for Home Assistant: the device makes its entities with MQTT discovery, publishes its data
// and gets the eco mode commands.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool room_valid;
    float room_temperature;
    float room_humidity;
    int battery_percent;    // -1 when not known
    int battery_mv;         // -1 when not known
    bool usb_power;
    int rssi;               // 0 when not connected
} hass_state_t;

typedef void (*hass_change_cb_t)(void);

// A command from Home Assistant: key "eco" or "alert_sound" (value "ON"/"OFF"), "eco_min" (minutes)
// or "volume" (0..100). It runs in the MQTT task: keep it short.
typedef void (*hass_command_cb_t)(const char *key, const char *value);

// Start the MQTT client. If the broker address is empty, MQTT stays off.
// on_change runs when the connection changes. It runs in the MQTT task: keep it short.
esp_err_t hass_start(const settings_t *settings, const char *version, hass_change_cb_t on_change,
                     hass_command_cb_t on_command);

bool hass_enabled(void);
bool hass_connected(void);

// Publish the data. Nothing happens when MQTT is not connected.
void hass_publish(const hass_state_t *state);

// Set the state of a control that Home Assistant shows ("eco", "eco_min", "alert_sound", "volume").
// It is published now and after each connection.
void hass_set_state(const char *key, const char *value);

// A control changed on the device (on the settings page): write the value also to the command topic,
// retained. Else the retained command of Home Assistant changes the value back after the next connection.
// Without a connection the value waits for the next one.
void hass_set_command(const char *key, const char *value);

// Disconnect before Wi-Fi goes off, and connect again after Wi-Fi is on.
// A clean disconnection keeps the entities available in Home Assistant.
void hass_suspend(void);
void hass_resume(void);

// Return a short text about the last MQTT error, or "".
const char *hass_last_error(void);

#ifdef __cplusplus
}
#endif
