// Device settings in NVS. A value that is not in NVS comes from the build configuration
// (sdkconfig.secrets / menuconfig), so a new device works with the built-in values.
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];     // main Wi-Fi network
    char wifi_pass[65];
    char wifi2_ssid[33];    // backup Wi-Fi network
    char wifi2_pass[65];
    char dns1[16];          // DNS servers instead of the DHCP ones, empty = from DHCP
    char dns2[16];
    char mqtt_uri[128];     // for example mqtt://homeassistant.local:1883, empty = no MQTT
    char mqtt_user[64];
    char mqtt_pass[64];
    char mqtt_prefix[32];   // Home Assistant discovery prefix
    char web_pass[64];      // password of the settings page on the home network
    char ap_pass[16];       // password of the setup access point, made once

    // Home Assistant data. An entity list has the main entity first, then the fallback ones,
    // separated by commas.
    char ha_url[96];        // for example http://homeassistant.local:8123, empty = no data from Home Assistant
    char ha_token[256];     // long-lived access token
    char e_temp[160];       // outdoor temperature
    char e_hum[160];        // outdoor humidity
    char e_aqi[160];        // air quality index
    char e_volt[96];        // UPS input voltage
    char e_ups[96];         // UPS status (NUT): "OL" = input power, "OB" = battery
    char e_drone[96];       // drone alert (on/off)
    char e_missile[96];     // missile alert (on/off)
    char e_off[96];         // next power off (time, or time to wait)
    char e_on[96];          // next power on
    char e_dtek[96];        // grid power from DTEK (on/off), empty = not known
    char hc_key[64];        // healthchecks.io read-only API key: the check gives the DTEK power
    char hc_check[64];      // the check: slug, UUID or unique_key
    char ping_host[64];     // a device without backup power (IP or name): it answers only with DTEK power
    char pw_queue[48];      // power queue label, for example "черга 1.1"
    char meteo[16];         // meteo.gov.ua station for the forecast, empty = no forecast
    char alert_reg[64];     // the region of ubilling.net.ua for the alerts without Home Assistant entities, "" = none
    char srv_url[128];      // the image of the "server" screen (trmnl-server, a 400 x 300 1-bit BMP)
    char srv_min[12];       // minutes between the sessions of the "server" screen, 1..30

    char screen[16];        // the screen after the start: "dashboard", "room", "note", "quad", "server", "clock"
    char note[512];         // the text of the "note" screen (UTF-8, lines with '\n')
    char scr_back[16];      // the screen before the low-battery clock, "" = none (USB power gives it back)

    // Eco mode. Home Assistant sets it through MQTT (for example, when nobody is at home).
    char eco[4];            // "1" = on, "0" = off
    char eco_min[12];       // minutes between the updates in eco mode

    // Sound
    char snd_alert[4];      // "1": a sound when an alert starts and ends, "0": no sound
    char snd_vol[12];       // volume, 0..100
    char snd_missile[12];   // the sound when a missile alert starts: see audio_sound_from_id()
    char snd_drone[12];     // the sound when a drone alert starts
    char snd_clear[12];     // the sound when all alerts end

    char ota_url[256];      // the last URL of a firmware archive (update from the settings page)
    char host[64];          // the device name on the home network, for example "rlcd.local" (DHCP, mDNS, HTTPS)
    char lang[8];           // the language of the screen, the page and Home Assistant: "uk", "en"
    // The answers of the first-start wizard: "1" yes, "0" no, "" = not asked yet (the page shows the wizard).
    char has_ha[4];         // Home Assistant: without it the screens with its data are not available
    char has_srv[4];        // trmnl-server: without it the server screen is not available

    // The time zone from Home Assistant (seconds east of UTC, UTC times); "" = no answer yet: CONFIG_RLCD_TZ.
    char tz_off[12];        // the offset now
    char tz_next[16];       // the next change of the offset, 0 = none
    char tz_noff[12];       // the offset after the change
    char tz_chk[16];        // the time of the last answer
} settings_t;

#define SCREEN_DASHBOARD "dashboard"
#define SCREEN_ROOM      "room"

// Read the settings. Use the build values for the fields that are not in NVS.
esp_err_t settings_load(settings_t *s);

// Write all fields to NVS.
esp_err_t settings_save(const settings_t *s);

// Erase the settings from NVS. The build values apply after the next start.
esp_err_t settings_reset(void);

// Write one field to NVS, for example "screen".
esp_err_t settings_save_field(const char *key, const char *value);

// Return false for a screen (its id, for example "dashboard") that needs a service the user does not have:
// "dashboard" and "quad" need Home Assistant, "server" needs trmnl-server.
bool settings_screen_available(const settings_t *s, const char *id);

#ifdef __cplusplus
}
#endif
