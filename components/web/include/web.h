// Settings page of the device (HTTP, port 80).
// Through the setup access point the page opens without a password.
// On the home network it asks for the user "admin" and the settings page password.
// If that password is empty, the page opens without a password on the home network too.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// Write the device status as plain text lines, for the page.
typedef void (*web_status_fn_t)(char *buf, size_t len);

// Runs when a firmware update starts or stops. It runs in the HTTP server task: keep it short.
typedef void (*web_change_cb_t)(void);

// A screen button or a new note on the page: show this screen now ("dashboard", "room", "note").
// note is the new note text, or NULL. It runs in the HTTP server task: copy the values and return.
typedef void (*web_show_cb_t)(const char *screen, const char *note);

// A setting that applies at once, without a restart: "eco", "alert_sound" ("1"/"0"), "eco_min",
// "volume" (numbers), "snd_missile", "snd_drone", "snd_clear" (sound ids). The value is checked.
// It runs in the HTTP server task: copy the values and return.
typedef void (*web_set_cb_t)(const char *key, const char *value);

// The events of the alert sounds. They are also the slots of the uploaded files.
typedef enum {
    WEB_SOUND_MISSILE,
    WEB_SOUND_DRONE,
    WEB_SOUND_CLEAR,
} web_sound_event_t;

// A sound check button: play the sound of this event. The page sends the settings of its form first
// (on_set), and the sound plays with them.
typedef void (*web_sound_cb_t)(web_sound_event_t event);

// Wait until the device applies the settings from on_set and the sound check from on_sound.
// Then the next page shows the new values. It runs in the HTTP server task.
typedef void (*web_wait_cb_t)(void);

typedef struct {
    settings_t *settings;       // the settings of main; the page reads them and writes them to NVS
    const char *version;
    web_status_fn_t status;
    web_change_cb_t on_change;
    web_show_cb_t on_show;
    web_set_cb_t on_set;
    web_sound_cb_t on_sound;
    web_wait_cb_t wait_applied;
} web_config_t;

esp_err_t web_start(const web_config_t *config);

// Return true while a firmware update runs: an upload from the page or a download from a URL.
bool web_ota_active(void);

// The time of the last page request (esp_timer_get_time()), 0 = none. In eco mode the device stays
// awake while somebody uses the page.
int64_t web_last_request_us(void);

#ifdef __cplusplus
}
#endif
