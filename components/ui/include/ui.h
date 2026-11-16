// Screens, 400 x 300, black and white:
// - dashboard: the compact screen of trmnl-server with the Home Assistant data;
// - room: a clock and the room climate (SHTC3);
// - server: four quadrants from trmnl-server; the device draws the date, the time and its icons;
// - clock: the autonomous mode: the date, the time, the room climate and the battery, without Wi-Fi;
// - days: the extreme power saving below 2 % battery: the date and the days of the year (not in the cycle);
// - setup: how to connect to the setup access point (it replaces the other screens while the access point is on).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "hadata.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_DASHBOARD = 0,
    UI_SCREEN_ROOM,
    UI_SCREEN_NOTE,         // a text from the settings page
    UI_SCREEN_QUAD,         // four quadrants: time, outdoor air, alert, power
    UI_SCREEN_SERVER,       // four quadrants from trmnl-server, the time quadrant from the device
    UI_SCREEN_CLOCK,        // the autonomous mode: no Wi-Fi, no MQTT, no Home Assistant data
    UI_SCREEN_COUNT,
    // Not in the KEY cycle, not in the settings, not kept: the extreme power saving below 2 % battery.
    UI_SCREEN_DAYS,
} ui_screen_t;

typedef struct {
    ui_screen_t screen;
    time_t now;                 // 0 when the time is not known
    const ha_data_t *ha;        // Home Assistant data, or NULL
    int ha_max_age_s;           // older data is not shown (eco mode allows older data)
    bool eco;                   // eco mode: Wi-Fi is off between the updates
    bool eco_label;             // show "ЕКО": only in the first minute after the switch
    const char *power_queue;    // power queue label, for example "черга 1.1"
    const char *note;           // the text of the note screen
    const uint8_t *server_image; // the image of the server screen (the canvas format), or NULL
    time_t server_image_at;     // the time of the server image
    bool room_valid;
    float room_temperature;     // degrees Celsius
    float room_humidity;        // percent
    int battery_mv;             // -1 when not known
    int battery_percent;        // -1 when not known
    bool usb_power;
    bool wifi_configured;
    bool wifi_connected;
    int wifi_rssi;              // dBm
    int wifi_reason;            // last disconnection reason, 0 = none
    bool ap_active;
    const char *ap_ssid;
    const char *ap_password;
    const char *ap_ip;
    const char *message;        // a message over the screen (for example, a reset), or NULL

    // Only for the settings page (the screen does not show them).
    const char *time_source;    // "NTP", "DS3231", "PCF85063" or NULL
    const char *ip;             // "" when not connected
    bool mqtt_enabled;
    bool mqtt_connected;
    const char *mqtt_error;     // "" when there is no error
    const char *version;        // firmware version
} ui_state_t;

// Return the screen id for the settings ("dashboard", "room", "note", "quad", "server", "clock").
const char *ui_screen_id(ui_screen_t screen);

// Return the screen for a settings id. An unknown id gives the dashboard.
ui_screen_t ui_screen_from_id(const char *id);

// Return the name of the screen for Home Assistant and the page, for example "З сервера".
const char *ui_screen_name(ui_screen_t screen);

// Return the screen for a name (or a settings id). An unknown text gives UI_SCREEN_COUNT.
ui_screen_t ui_screen_from_name(const char *name);

// Draw the screen on the gfx canvas.
void ui_render(const ui_state_t *state);

// The power source as the screen shows it, for the settings page: "Світло від ДТЕК",
// "Світло від генератора" or "Світло від ДБЖ".
void ui_power_text(const ui_state_t *state, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
