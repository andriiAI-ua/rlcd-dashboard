// rlcd-dashboard: a standalone dashboard for the Waveshare ESP32-S3-RLCD-4.2.
// The device collects the data and draws the screen itself. It needs no server.
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "git_version.h"
#include "hadata.h"
#include "audio.h"
#include "hass.h"
#include "net.h"
#include "nvs_flash.h"
#include "rtc_clock.h"
#include "settings.h"
#include "shtc3.h"
#include "srvimg.h"
#include "st7305.h"
#include "texts.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "main";

// The time is not known before this moment (2024-01-01 00:00 UTC).
#define VALID_TIME 1704067200

// Without a VBUS signal, the battery voltage shows a charger: a rise of CHARGE_RISE_MV over the lowest voltage
// on battery means a charge current (on battery the voltage only falls); a fall of CHARGE_END_MV under the
// highest voltage of the charge means its end. With Wi-Fi on the voltage is lower (the load) than with Wi-Fi
// off, so each state has its own readings, its lowest and its highest voltage: else the higher voltage in
// the sleep of the clock or the server screen looks like a charge, and a median of the other state, which is
// older and lower during the charge, looks like its end. A full battery at rest is not USB power: it shows
// its percent.
#define CHARGE_RISE_MV 80
#define CHARGE_END_MV  30
// On battery the percent goes up only by this step or more: the voltage changes a little with the load.
#define PERCENT_UP_STEP 5
// The charge current goes through the resistance of the cell and the wires, so on charge the voltage is higher
// than the charge shows: at the start of the charge it jumps by 0.2-0.3 V. From about 80 % the charger keeps
// 4.2 V and its current falls. The estimate on charge: the voltage before the charge plus the rise since its
// start; from CHARGE_CV_MV it goes up from CHARGE_CV_PERCENT or more by 1 % in CHARGE_CV_MIN_PER_PERCENT minutes,
// to 100 %.
#define CHARGE_CV_MV              4180
#define CHARGE_CV_PERCENT         80    // the charge at the start of the constant voltage, at least
#define CHARGE_CV_MIN_PER_PERCENT 3

// Home Assistant data older than this is not shown in the normal mode.
#define HA_MAX_AGE_S (5 * 60)

static TaskHandle_t s_main_task;

// The version with the git revision of the source, for example "0.1.0 (75cd8c4)", or "0.1.0 (75cd8c4-dirty)"
// for a build with changes that are not committed. Without git it is the version only.
static char s_version[48];

static const char *full_version(void)
{
    if (!s_version[0]) {
        const char *version = esp_app_get_description()->version;
        if (RLCD_GIT_HASH[0]) {
            snprintf(s_version, sizeof(s_version), "%s (%s%s)", version, RLCD_GIT_HASH,
                     RLCD_GIT_DIRTY ? "-dirty" : "");
        } else {
            strlcpy(s_version, version, sizeof(s_version));
        }
    }
    return s_version;
}
static settings_t s_settings;
static bool s_have_shtc3;

// The last screen data, for the status on the settings page.
static ui_state_t s_last;
static ha_data_t s_last_ha;
static char s_last_ip[16];

static void wake_main_task(void)
{
    xTaskNotifyGive(s_main_task);
}

/* Check of a new firmware after an update (OTA) */

// A new firmware has this time to show the screen at least two times (the start and the next minute).
#define OTA_CHECK_US (90LL * 1000 * 1000)

static volatile unsigned s_screens_shown;
static volatile bool s_ota_pending;   // the check runs: no eco sleep until it ends

static void ota_check(void *arg)
{
    (void)arg;
    if (s_screens_shown >= 2) {
        ESP_LOGI(TAG, "The new firmware works: keep it");
        esp_ota_mark_app_valid_cancel_rollback();
        s_ota_pending = false;
    } else {
        ESP_LOGE(TAG, "The new firmware does not update the screen: go back to the previous firmware");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

// After an update the bootloader starts the new firmware once, in the state "pending verify".
// If the firmware does not confirm itself, the bootloader starts the previous firmware after the next reset.
static void start_ota_check(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "New firmware in %s: check it for %lld s", running->label, OTA_CHECK_US / 1000000);
    const esp_timer_create_args_t args = { .callback = ota_check, .name = "ota_check" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK && esp_timer_start_once(timer, OTA_CHECK_US) == ESP_OK) {
        s_ota_pending = true;
    }
}

/* Eco mode */

#define ECO_MIN_MINUTES     5
#define ECO_MAX_MINUTES     240
#define ECO_WIFI_WAIT_MS    25000   // wait for Wi-Fi after the wakeup
#define ECO_DATA_WAIT_MS    15000   // wait for the Home Assistant data
#define ECO_MQTT_WAIT_MS    3000    // time for the MQTT messages before Wi-Fi goes off
#define ECO_HOLD_WAIT_MS    12000   // wait for the release of a held button
#define ECO_AWAKE_MS        120000  // Wi-Fi stays on after BOOT and after a page request
#define IMAGE_TRIES         3       // the image of the server screen: tries in a session
#define IMAGE_RETRY_MS      2000    // the pause between the tries
#define MODE_LABEL_US       (60 * 1000000LL)   // "ЕКО" stays on the screen after the switch for this time
#define SERVER_MIN_MINUTES  1       // the sessions of the server screen: every 1 to 30 minutes
#define SERVER_MAX_MINUTES  30

// Commands from Home Assistant (MQTT task) and from the settings page (HTTP task).
// The main task applies them. -1 = no command.
static int s_cmd_eco = -1;
static int s_cmd_eco_min = -1;
static int s_cmd_alert_sound = -1;
static int s_cmd_volume = -1;
static int s_cmd_srv_min = -1;
static int s_cmd_screen = -1;   // a ui_screen_t from Home Assistant (apply_show_request() applies it)
static int s_cmd_city = -1;     // a city of the forecast (hadata_city_*), hadata_city_count() = no forecast
static int s_cmd_region = -1;   // a region of the alerts (hadata_region_*), hadata_region_count() = none

// The commands that came from the page. The main task writes them also to the MQTT command topics.
enum { CMD_ECO = 1, CMD_ECO_MIN = 2, CMD_ALERT_SOUND = 4, CMD_VOLUME = 8, CMD_SRV_MIN = 16, CMD_CITY = 32,
       CMD_REGION = 64 };
static uint32_t s_cmd_local;

static void set_command(const char *key, const char *value, bool local)
{
    // Home Assistant sends "ON"/"OFF" and a number as "30" or as "30.0"; the page sends "1"/"0".
    const long number = lround(strtod(value, NULL));
    const int on = strcasecmp(value, "ON") == 0 || strcmp(value, "1") == 0 ? 1 : 0;
    uint32_t bit = 0;
    if (strcmp(key, "eco") == 0) {
        __atomic_store_n(&s_cmd_eco, on, __ATOMIC_RELAXED);
        bit = CMD_ECO;
    } else if (strcmp(key, "eco_min") == 0) {
        if (number >= ECO_MIN_MINUTES && number <= ECO_MAX_MINUTES) {
            __atomic_store_n(&s_cmd_eco_min, (int)number, __ATOMIC_RELAXED);
            bit = CMD_ECO_MIN;
        }
    } else if (strcmp(key, "alert_sound") == 0) {
        __atomic_store_n(&s_cmd_alert_sound, on, __ATOMIC_RELAXED);
        bit = CMD_ALERT_SOUND;
    } else if (strcmp(key, "volume") == 0) {
        if (number >= 0 && number <= 100) {
            __atomic_store_n(&s_cmd_volume, (int)number, __ATOMIC_RELAXED);
            bit = CMD_VOLUME;
        }
    } else if (strcmp(key, "srv_min") == 0) {
        if (number >= SERVER_MIN_MINUTES && number <= SERVER_MAX_MINUTES) {
            __atomic_store_n(&s_cmd_srv_min, (int)number, __ATOMIC_RELAXED);
            bit = CMD_SRV_MIN;
        }
    } else if (strcmp(key, "meteo") == 0) {
        // The page sends a station ("" = no forecast), Home Assistant sends a name of its list.
        int city = hadata_city_find(value);
        if (city < 0) {
            city = hadata_city_find_name(value);
        }
        if (city < 0 && (value[0] == '\0' || strcmp(value, tr(T_METEO_NONE)) == 0)) {
            city = hadata_city_count();
        }
        if (city >= 0) {
            __atomic_store_n(&s_cmd_city, city, __ATOMIC_RELAXED);
            bit = CMD_CITY;
        }
    } else if (strcmp(key, "alert_reg") == 0) {
        // The page sends the number of the region ("" = none), Home Assistant sends a name of its list.
        int region = hadata_region_find_name(value);
        if (region < 0 && value[0] >= '0' && value[0] <= '9' && number < hadata_region_count()) {
            region = (int)number;
        }
        if (region < 0 && (value[0] == '\0' || strcmp(value, tr(T_ALERTS_NONE)) == 0)) {
            region = hadata_region_count();
        }
        if (region >= 0) {
            __atomic_store_n(&s_cmd_region, region, __ATOMIC_RELAXED);
            bit = CMD_REGION;
        }
    } else if (strcmp(key, "screen") == 0) {
        // Only from Home Assistant: the page sends its screen requests to on_show().
        const ui_screen_t screen = ui_screen_from_name(value);
        if (screen < UI_SCREEN_COUNT) {
            __atomic_store_n(&s_cmd_screen, (int)screen, __ATOMIC_RELAXED);
        }
    }
    if (local && bit) {
        __atomic_fetch_or(&s_cmd_local, bit, __ATOMIC_RELAXED);
    }
    wake_main_task();
}

static void on_command(const char *key, const char *value)
{
    set_command(key, value, false);
}

static bool alert_sound_on(void)
{
    return s_settings.snd_alert[0] == '1';
}

static int volume(void)
{
    const int v = atoi(s_settings.snd_vol);
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

// Minutes between the sessions of the server screen.
static int server_minutes(void)
{
    const int m = atoi(s_settings.srv_min);
    return m < SERVER_MIN_MINUTES ? SERVER_MIN_MINUTES : (m > SERVER_MAX_MINUTES ? SERVER_MAX_MINUTES : m);
}

// The city of the forecast as Home Assistant shows it: a name of the list, or "no forecast".
// A station out of the list (an old setting) shows its number.
static const char *city_name(void)
{
    if (!s_settings.meteo[0]) {
        return tr(T_METEO_NONE);
    }
    const int city = hadata_city_find(s_settings.meteo);
    return city >= 0 ? hadata_city_name(city) : s_settings.meteo;
}

// The region of the alerts as Home Assistant shows it: a name of the list, or "not set".
static const char *region_name(void)
{
    const int region = hadata_region_find(s_settings.alert_reg);
    return region >= 0 ? hadata_region_name(region) : tr(T_ALERTS_NONE);
}

// Publish the states of the controls, so that Home Assistant shows the values of the device.
static void publish_controls(void)
{
    char buf[12];
    hass_set_state("eco", s_settings.eco[0] == '1' ? "ON" : "OFF");
    snprintf(buf, sizeof(buf), "%d", atoi(s_settings.eco_min));
    hass_set_state("eco_min", buf);
    hass_set_state("alert_sound", alert_sound_on() ? "ON" : "OFF");
    snprintf(buf, sizeof(buf), "%d", volume());
    hass_set_state("volume", buf);
    snprintf(buf, sizeof(buf), "%d", server_minutes());
    hass_set_state("srv_min", buf);
    hass_set_state("screen", ui_screen_name(ui_screen_from_id(s_settings.screen)));
    hass_set_state("meteo", city_name());
    hass_set_state("alert_reg", region_name());
}

// Tell Home Assistant the screen. A change on the device (KEY, the page) goes also to the command topic:
// else the retained command of Home Assistant changes the screen back at the next connection.
static void publish_screen(ui_screen_t screen, bool local)
{
    hass_set_state("screen", ui_screen_name(screen));
    if (local) {
        hass_set_command("screen", ui_screen_name(screen));
    }
}

// A screen choice (KEY, the page, Home Assistant) cancels the return after the low-battery clock.
static void forget_screen_back(void)
{
    if (s_settings.scr_back[0]) {
        s_settings.scr_back[0] = '\0';
        settings_save_field("scr_back", s_settings.scr_back);
        ESP_LOGI(TAG, "Low battery: a new screen choice, no return");
    }
}

static bool eco_on(void)
{
    return s_settings.eco[0] == '1';
}

static int eco_minutes(void)
{
    const int m = atoi(s_settings.eco_min);
    return m < ECO_MIN_MINUTES ? ECO_MIN_MINUTES : (m > ECO_MAX_MINUTES ? ECO_MAX_MINUTES : m);
}

// Home Assistant or the settings page turns eco mode on and off. It waits while the access point is on,
// while a firmware update runs and while a new firmware is in its check.
static bool eco_active(void)
{
    return eco_on() && !net_ap_active() && !web_ota_active() && !s_ota_pending;
}

// The clock screen is the autonomous mode: no Wi-Fi, no MQTT, no Home Assistant data. It waits while
// the access point is on, while a firmware update runs and while a new firmware is in its check.
static bool clock_active(ui_screen_t screen)
{
    return screen == UI_SCREEN_CLOCK && !net_ap_active() && !web_ota_active() && !s_ota_pending;
}

// The "З сервера" screen is the semi-autonomous mode: Wi-Fi and MQTT only every 5 minutes. It waits in
// the same cases as the clock.
static bool server_active(ui_screen_t screen)
{
    return screen == UI_SCREEN_SERVER && !net_ap_active() && !web_ota_active() && !s_ota_pending;
}

// True while the clock or the server screen keeps Wi-Fi and MQTT off.
static bool s_offline;

// Below EXTREME_BATTERY %: the extreme power saving with the days-in-year screen. Not kept in the settings:
// the next start reads the battery again.
#define EXTREME_BATTERY  2
#define EXTREME_CHECK_MS (10 * 60 * 1000)   // the battery check (USB power) in the extreme power saving
static bool s_extreme;

// Eco mode: no sleep before this time (esp_timer_get_time()). BOOT sets it.
static int64_t s_awake_until;

// After BOOT on the clock or the server screen: no light sleep for this time.
#define USB_WINDOW_MS 30000
static int64_t s_usb_window_until;

// The USB of the chip does not work in light sleep. A computer does not see a device that sleeps, and the device
// sees the computer only after the computer sees it. So do not sleep with USB data (the log), with USB power
// (a charge, see read_battery()) and for USB_WINDOW_MS after BOOT.
static bool may_sleep(void)
{
    return !board_usb_data_connected() && !s_last.usb_power && esp_timer_get_time() >= s_usb_window_until;
}

// In eco mode, keep Wi-Fi on for some time after BOOT and while somebody uses the settings page.
static bool keep_awake(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t page = web_last_request_us();
    return now < s_awake_until || (page > 0 && now - page < ECO_AWAKE_MS * 1000LL);
}

// Apply the commands from Home Assistant and from the page. Return true if something changed.
static bool apply_commands(void)
{
    bool changed = false;
    const uint32_t local = __atomic_exchange_n(&s_cmd_local, 0, __ATOMIC_RELAXED);
    const int eco = __atomic_exchange_n(&s_cmd_eco, -1, __ATOMIC_RELAXED);
    if (eco >= 0 && eco != (int)eco_on()) {
        strlcpy(s_settings.eco, eco ? "1" : "0", sizeof(s_settings.eco));
        settings_save_field("eco", s_settings.eco);
        ESP_LOGI(TAG, "Eco mode is %s", eco ? "on" : "off");
        changed = true;
    }
    const int minutes = __atomic_exchange_n(&s_cmd_eco_min, -1, __ATOMIC_RELAXED);
    if (minutes > 0 && minutes != eco_minutes()) {
        snprintf(s_settings.eco_min, sizeof(s_settings.eco_min), "%d", minutes);
        settings_save_field("eco_min", s_settings.eco_min);
        ESP_LOGI(TAG, "Eco mode: update every %d min", minutes);
        changed = true;
    }
    const int sound = __atomic_exchange_n(&s_cmd_alert_sound, -1, __ATOMIC_RELAXED);
    if (sound >= 0 && sound != (int)alert_sound_on()) {
        strlcpy(s_settings.snd_alert, sound ? "1" : "0", sizeof(s_settings.snd_alert));
        settings_save_field("snd_alert", s_settings.snd_alert);
        ESP_LOGI(TAG, "Alert sound is %s", sound ? "on" : "off");
        changed = true;
    }
    const int srv = __atomic_exchange_n(&s_cmd_srv_min, -1, __ATOMIC_RELAXED);
    if (srv > 0 && srv != server_minutes()) {
        snprintf(s_settings.srv_min, sizeof(s_settings.srv_min), "%d", srv);
        settings_save_field("srv_min", s_settings.srv_min);
        ESP_LOGI(TAG, "Server screen: a session every %d min", srv);
        changed = true;
    }
    const int city = __atomic_exchange_n(&s_cmd_city, -1, __ATOMIC_RELAXED);
    if (city >= 0) {
        const char *station = hadata_city_station(city);   // "" for "no forecast"
        if (strcmp(station, s_settings.meteo) != 0) {
            strlcpy(s_settings.meteo, station, sizeof(s_settings.meteo));
            settings_save_field("meteo", s_settings.meteo);
            hadata_set_station(s_settings.meteo);
            changed = true;
        }
    }
    const int region = __atomic_exchange_n(&s_cmd_region, -1, __ATOMIC_RELAXED);
    if (region >= 0) {
        const char *key = hadata_region_key(region);   // "" for "not set"
        if (strcmp(key, s_settings.alert_reg) != 0) {
            strlcpy(s_settings.alert_reg, key, sizeof(s_settings.alert_reg));
            settings_save_field("alert_reg", s_settings.alert_reg);
            hadata_set_region(s_settings.alert_reg);
            changed = true;
        }
    }
    const int vol = __atomic_exchange_n(&s_cmd_volume, -1, __ATOMIC_RELAXED);
    if (vol >= 0 && vol != volume()) {
        snprintf(s_settings.snd_vol, sizeof(s_settings.snd_vol), "%d", vol);
        settings_save_field("snd_vol", s_settings.snd_vol);
        audio_set_volume(vol);
        ESP_LOGI(TAG, "Volume %d", vol);
        changed = true;
    }
    if (changed) {
        publish_controls();
    }
    // A change on the page goes also to the command topics: else the retained command of Home Assistant
    // changes it back after the next connection.
    char buf[12];
    if (local & CMD_ECO) {
        hass_set_command("eco", eco_on() ? "ON" : "OFF");
    }
    if (local & CMD_ECO_MIN) {
        snprintf(buf, sizeof(buf), "%d", eco_minutes());
        hass_set_command("eco_min", buf);
    }
    if (local & CMD_CITY) {
        hass_set_command("meteo", city_name());
    }
    if (local & CMD_REGION) {
        hass_set_command("alert_reg", region_name());
    }
    if (local & CMD_ALERT_SOUND) {
        hass_set_command("alert_sound", alert_sound_on() ? "ON" : "OFF");
    }
    if (local & CMD_VOLUME) {
        snprintf(buf, sizeof(buf), "%d", volume());
        hass_set_command("volume", buf);
    }
    if (local & CMD_SRV_MIN) {
        snprintf(buf, sizeof(buf), "%d", server_minutes());
        hass_set_command("srv_min", buf);
    }
    // In eco mode the access point does not start by itself (the router can be off).
    net_set_auto_ap(!eco_on());
    return changed;
}

/* Alert sounds */

static const char *const SOUND_KEYS[] = { "snd_missile", "snd_drone", "snd_clear" };
static const audio_sound_t DEFAULT_SOUNDS[] = { AUDIO_SOUND_SIREN, AUDIO_SOUND_WARBLE, AUDIO_SOUND_CHIME };

static char *event_sound(web_sound_event_t event)
{
    return event == WEB_SOUND_MISSILE ? s_settings.snd_missile
                                      : (event == WEB_SOUND_DRONE ? s_settings.snd_drone : s_settings.snd_clear);
}

// Play the sound of an event. The uploaded file of an event is in the slot with the same number.
static void play_event(web_sound_event_t event)
{
    audio_play(audio_sound_from_id(event_sound(event), DEFAULT_SOUNDS[event]), (int)event);
}

// Play a sound when an alert starts (or a new kind of alert joins) and when all alerts end.
// A missile alert has priority over a drone alert. Only fresh data counts. The first known state after
// the start gives no sound. Eco mode gives no sound: nobody is at home.
static void alert_sound(const ui_state_t *state, const ha_data_t *ha)
{
    static int previous = -1;   // bit 0: drone, bit 1: missile; -1: not known yet
    if (!ha->alert_known || !ha->updated || !state->now || state->now - ha->updated > state->ha_max_age_s) {
        return;
    }
    const int current = (ha->drone ? 1 : 0) | (ha->missile ? 2 : 0);
    const int before = previous;
    previous = current;
    if (before < 0 || current == before || !alert_sound_on() || state->eco) {
        return;
    }
    const int started = current & ~before;
    if (started) {
        const web_sound_event_t event = (started & 2) ? WEB_SOUND_MISSILE : WEB_SOUND_DRONE;
        ESP_LOGI(TAG, "%s alert starts: play its sound", event == WEB_SOUND_MISSILE ? "Missile" : "Drone");
        play_event(event);
    } else if (current == 0) {
        ESP_LOGI(TAG, "All alerts end: play the all-clear sound");
        play_event(WEB_SOUND_CLEAR);
    }
}

/* Screen requests from the settings page */

// The HTTP server task writes a request, the main task applies it. -1 = no screen request.
static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_request_screen = -1;
static bool s_request_note;
static char s_request_note_text[sizeof(((settings_t *)0)->note)];
static char s_request_sound[3][sizeof(((settings_t *)0)->snd_missile)];   // "" = no request
static int s_request_check = -1;   // a sound check button; -1 = no request

// The page counts its requests; the main task keeps the count that it applied.
// The page waits for the main task, so the next page shows the new values.
#define PAGE_WAIT_MS 3000
static uint32_t s_page_requests;
static uint32_t s_page_applied;

static void page_request_done(void)
{
    __atomic_fetch_add(&s_page_requests, 1, __ATOMIC_RELEASE);
    wake_main_task();
}

// A setting from the page that applies at once. The page has checked the value.
static void on_page_set(const char *key, const char *value)
{
    bool sound = false;
    for (int i = 0; i < 3; i++) {
        if (strcmp(key, SOUND_KEYS[i]) == 0) {
            taskENTER_CRITICAL(&s_request_lock);
            strlcpy(s_request_sound[i], value, sizeof(s_request_sound[i]));
            taskEXIT_CRITICAL(&s_request_lock);
            sound = true;
            break;
        }
    }
    if (!sound) {
        set_command(key, value, true);
    }
    page_request_done();
}

// A sound check button of the settings page (HTTP server task). The main task plays the sound
// after it applies the settings of the same form.
static void sound_check(web_sound_event_t event)
{
    taskENTER_CRITICAL(&s_request_lock);
    s_request_check = (int)event;
    taskEXIT_CRITICAL(&s_request_lock);
    page_request_done();
}

// The HTTP server task waits until the main task applies the requests of the page (or the time ends).
static void wait_page_applied(void)
{
    const uint32_t target = __atomic_load_n(&s_page_requests, __ATOMIC_ACQUIRE);
    for (int ms = 0; ms < PAGE_WAIT_MS; ms += 20) {
        if ((int32_t)(__atomic_load_n(&s_page_applied, __ATOMIC_ACQUIRE) - target) >= 0) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "Page: the settings apply later");
}

// Keep the sound choices from the page.
static void apply_sound_requests(void)
{
    for (int i = 0; i < 3; i++) {
        char value[sizeof(s_request_sound[i])];
        taskENTER_CRITICAL(&s_request_lock);
        strlcpy(value, s_request_sound[i], sizeof(value));
        s_request_sound[i][0] = '\0';
        taskEXIT_CRITICAL(&s_request_lock);
        char *field = event_sound((web_sound_event_t)i);
        if (value[0] && strcmp(field, value) != 0) {
            strlcpy(field, value, sizeof(s_settings.snd_missile));
            settings_save_field(SOUND_KEYS[i], field);
            ESP_LOGI(TAG, "Page: %s = %s", SOUND_KEYS[i], field);
        }
    }
}

// Apply the commands from Home Assistant and the requests from the page. A sound check plays last:
// then it has the new volume and the new sound.
static void apply_requests(void)
{
    const uint32_t requests = __atomic_load_n(&s_page_requests, __ATOMIC_ACQUIRE);
    apply_commands();
    apply_sound_requests();
    taskENTER_CRITICAL(&s_request_lock);
    const int check = s_request_check;
    s_request_check = -1;
    taskEXIT_CRITICAL(&s_request_lock);
    if (check >= 0) {
        play_event((web_sound_event_t)check);
    }
    __atomic_store_n(&s_page_applied, requests, __ATOMIC_RELEASE);
}

static void on_show(const char *screen, const char *note)
{
    taskENTER_CRITICAL(&s_request_lock);
    if (note) {
        strlcpy(s_request_note_text, note, sizeof(s_request_note_text));
        s_request_note = true;
    }
    s_request_screen = (int)ui_screen_from_id(screen);
    taskEXIT_CRITICAL(&s_request_lock);
    wake_main_task();
}

// A screen that needs a service the user does not have (see the first-start wizard) is not available.
static bool screen_available(ui_screen_t screen)
{
    return settings_screen_available(&s_settings, ui_screen_id(screen));
}

// Apply a screen request from the page or from Home Assistant. Keep the screen and the note for the next start.
// Return true if the screen must change.
static bool apply_show_request(ui_screen_t *screen)
{
    static char note[sizeof(s_request_note_text)];
    taskENTER_CRITICAL(&s_request_lock);
    const int page = s_request_screen;
    const bool has_note = s_request_note;
    if (has_note) {
        memcpy(note, s_request_note_text, sizeof(note));
    }
    s_request_screen = -1;
    s_request_note = false;
    taskEXIT_CRITICAL(&s_request_lock);
    const int ha = __atomic_exchange_n(&s_cmd_screen, -1, __ATOMIC_RELAXED);

    if (has_note) {
        strlcpy(s_settings.note, note, sizeof(s_settings.note));
        settings_save_field("note", s_settings.note);
    }
    const bool from_page = page >= 0;
    const int requested = from_page ? page : ha;
    if (requested < 0) {
        return has_note;
    }
    if (!screen_available((ui_screen_t)requested)) {
        ESP_LOGW(TAG, "The screen \"%s\" needs a service that is not set: keep the screen",
                 ui_screen_id((ui_screen_t)requested));
        publish_screen(*screen, true);
        return has_note;
    }
    if (!from_page && requested == UI_SCREEN_NOTE && s_settings.note[0] == '\0') {
        ESP_LOGW(TAG, "Home Assistant: the note screen has no text, keep the screen");
        publish_screen(*screen, true);
        return has_note;
    }
    // Home Assistant repeats its retained command at each connection: the same screen is no change.
    if ((ui_screen_t)requested == *screen) {
        publish_screen(*screen, from_page);
        return has_note;
    }
    *screen = (ui_screen_t)requested;
    forget_screen_back();
    if (strcmp(s_settings.screen, ui_screen_id(*screen)) != 0) {
        strlcpy(s_settings.screen, ui_screen_id(*screen), sizeof(s_settings.screen));
        settings_save_field("screen", s_settings.screen);
    }
    ESP_LOGI(TAG, "%s: screen \"%s\"", from_page ? "Page" : "Home Assistant", s_settings.screen);
    publish_screen(*screen, from_page);
    return true;
}

/* Helpers */

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* Battery */

// The charge of a Li-ion cell (18650) from its voltage at a small load. The curve is flat in the middle
// and steep at the ends: a straight line between two voltages shows too much charge.
static const struct {
    int mv;
    int percent;
} BATTERY_CURVE[] = {
    { 4190, 100 }, { 4080, 90 }, { 3980, 80 }, { 3890, 70 }, { 3810, 60 }, { 3750, 50 },
    { 3700, 40 },  { 3650, 30 }, { 3600, 20 }, { 3530, 10 }, { 3450, 5 },  { 3300, 0 },
};

static int curve_percent(int mv)
{
    const int n = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);
    if (mv >= BATTERY_CURVE[0].mv) {
        return 100;
    }
    for (int i = 1; i < n; i++) {
        if (mv >= BATTERY_CURVE[i].mv) {
            const int dmv = BATTERY_CURVE[i - 1].mv - BATTERY_CURVE[i].mv;
            const int dp = BATTERY_CURVE[i - 1].percent - BATTERY_CURVE[i].percent;
            return BATTERY_CURVE[i].percent + (mv - BATTERY_CURVE[i].mv) * dp / dmv;
        }
    }
    return 0;
}

static int median3(int a, int b, int c)
{
    if (a > b) {
        const int t = a;
        a = b;
        b = t;
    }
    return c < a ? a : (c > b ? b : c);
}

// The readings of one load state (Wi-Fi off or on).
typedef struct {
    int samples[3];     // the last voltages, mV: the median ignores one reading at a high load
    unsigned count;
    int low;            // the lowest voltage on battery, mV; 0 = none
    int high;           // the highest voltage of the charge, mV; 0 = none
} bat_load_t;

// The battery between the updates (main task).
static struct {
    bat_load_t load[2]; // [0]: Wi-Fi off, [1]: Wi-Fi on
    bool charging;      // the voltage rises: a charger without USB data
    int percent;        // the last percent on battery; -1 = none
    int charge_base;    // the voltage on battery before the charge, mV; 0 = not known
    int charge_start;   // the voltage at the start of the charge, mV
    int charge_percent; // the last estimate on charge; -1 = none
    int64_t cv_since;   // the start of the constant voltage of the charger, us; 0 = not yet
    int cv_percent;     // the estimate at that start
} s_bat = { .percent = -1, .charge_percent = -1 };

// The charge on charge (see CHARGE_CV_MV); -1 if the voltage before the charge is not known.
static int charge_estimate(int mv)
{
    if (s_bat.charge_base == 0) {
        return -1;
    }
    int percent = curve_percent(s_bat.charge_base + mv - s_bat.charge_start);
    if (mv >= CHARGE_CV_MV) {
        const int64_t now = esp_timer_get_time();
        if (s_bat.cv_since == 0) {
            s_bat.cv_since = now;
            s_bat.cv_percent = percent > s_bat.charge_percent ? percent : s_bat.charge_percent;
            if (s_bat.cv_percent < CHARGE_CV_PERCENT) {
                s_bat.cv_percent = CHARGE_CV_PERCENT;
            }
        }
        const int minutes = (int)((now - s_bat.cv_since) / 60000000LL);
        percent = s_bat.cv_percent + minutes / CHARGE_CV_MIN_PER_PERCENT;
    }
    if (percent > 100) {
        percent = 100;
    }
    // On charge the estimate does not go down: the voltage changes a little with the load.
    if (percent < s_bat.charge_percent) {
        percent = s_bat.charge_percent;
    }
    s_bat.charge_percent = percent;
    return percent;
}

// Read the battery and find USB power. On USB power the percent is an estimate of the charge (see
// CHARGE_CV_MV), or -1 if the device did not see the start of the charge.
static void read_battery(ui_state_t *state)
{
    const int wifi = net_connected() ? 1 : 0;
    bat_load_t *b = &s_bat.load[wifi];
    // A sound takes much current: then keep the last readings.
    if (!audio_busy() || b->count == 0) {
        const int mv = board_battery_mv();
        if (mv > 0) {
            b->samples[b->count % 3] = mv;
            b->count++;
        }
    }
    const int vbus = board_vbus_sense();
    if (b->count == 0) {
        state->battery_mv = -1;
        state->battery_percent = -1;
        state->usb_power = vbus >= 0 ? vbus == 1 : board_usb_data_connected();
        return;
    }
    const bool three = b->count >= 3;
    const int mv = three ? median3(b->samples[0], b->samples[1], b->samples[2]) : b->samples[(b->count - 1) % 3];
    state->battery_mv = mv;

    if (!s_bat.charging) {
        if (b->low == 0 || mv < b->low) {
            b->low = mv;
        }
        if (three && mv >= b->low + CHARGE_RISE_MV) {
            s_bat.charging = true;
            s_bat.load[0].high = 0;
            s_bat.load[1].high = 0;
            b->high = mv;
            s_bat.charge_base = b->low;
            s_bat.charge_start = mv;
            s_bat.charge_percent = -1;
            s_bat.cv_since = 0;
            ESP_LOGI(TAG, "Battery: the voltage rises (%d -> %d mV), a charger is on", b->low, mv);
        }
    } else if (three) {
        // Compare the readings of one load state only. On the server screen the readings with Wi-Fi come
        // once in 5 min, and during the charge their median is lower than the newer one without Wi-Fi.
        if (mv > b->high) {
            b->high = mv;
        }
        if (mv <= b->high - CHARGE_END_MV) {
            s_bat.charging = false;
            ESP_LOGI(TAG, "Battery: the voltage falls (%d -> %d mV), no charger", b->high, mv);
            s_bat.charge_base = 0;
            s_bat.charge_percent = -1;
            s_bat.cv_since = 0;
            b->low = mv;
            // The readings of the other load state come from the charge: they are not the battery now.
            bat_load_t *other = &s_bat.load[1 - wifi];
            other->count = 0;
            other->low = 0;
        }
    }

    if (vbus >= 0) {
        state->usb_power = vbus == 1;
    } else {
        state->usb_power = board_usb_data_connected() || s_bat.charging;
    }
    if (state->usb_power) {
        s_bat.percent = -1;
        state->battery_percent = s_bat.charging ? charge_estimate(mv) : -1;
        return;
    }
    const int percent = curve_percent(mv);
    if (s_bat.percent < 0 || percent < s_bat.percent || percent >= s_bat.percent + PERCENT_UP_STEP) {
        s_bat.percent = percent;
    }
    state->battery_percent = s_bat.percent;
}

// Automatic light sleep between the Wi-Fi beacons, with a lower CPU frequency when nothing runs.
// Only on battery: on USB power the energy costs nothing, and the USB log needs a chip that does not sleep.
static int s_light_sleep = -1;  // -1 = not set yet

static void apply_power_mode(bool usb_power)
{
    const int sleep = usb_power ? 0 : 1;
    if (sleep == s_light_sleep) {
        return;
    }
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = sleep == 1,
    };
    const esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Power management: %s", esp_err_to_name(err));
        return;
    }
    s_light_sleep = sleep;
    ESP_LOGI(TAG, "Power management: automatic light sleep %s", sleep ? "on" : "off (USB power)");
}

/* Time zone */

// Daylight saving time: Home Assistant gives the UTC offset now, the next change and the offset after it
// (hadata_request_tz()). The settings keep them, and the device changes the offset itself at that moment,
// also without Wi-Fi (the clock). Without an answer of Home Assistant, CONFIG_RLCD_TZ gives the rules.
// The device asks again after a quarter, once in the last week before a change, and after the change.
#define TZ_CHECK_S (90 * 24 * 3600)
#define TZ_WEEK_S  (7 * 24 * 3600)
#define TZ_RETRY_S 3600             // after a request without an answer
#define TZ_WAIT_MS 5000             // a session waits for the answer

static bool tz_known(void)
{
    return s_settings.tz_off[0] != '\0';
}

// The offset (seconds east of UTC) at a moment: before the change the offset now, after it the next one.
static int tz_offset_at(time_t t)
{
    const time_t next = (time_t)atoll(s_settings.tz_next);
    return next && t >= next ? atoi(s_settings.tz_noff) : atoi(s_settings.tz_off);
}

// Use the offset from Home Assistant: a POSIX TZ with a fixed offset, for example "<+0300>-3:00".
static void apply_tz(void)
{
    static int applied = INT_MIN;
    if (!tz_known()) {
        return;
    }
    const int offset = tz_offset_at(time(NULL));
    if (offset == applied) {
        return;
    }
    const int a = offset < 0 ? -offset : offset;
    char tz[32];
    snprintf(tz, sizeof(tz), "<%c%02d%02d>%c%d:%02d", offset < 0 ? '-' : '+', a / 3600, a / 60 % 60,
             offset < 0 ? '+' : '-', a / 3600, a / 60 % 60);
    setenv("TZ", tz, 1);
    tzset();
    applied = offset;
    ESP_LOGI(TAG, "Time zone from Home Assistant: %s", tz);
}

static void save_tz_field(const char *key, char *field, size_t size, long long value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", value);
    if (strcmp(field, buf) != 0) {
        strlcpy(field, buf, size);
        settings_save_field(key, field);
    }
}

// Keep a new answer of Home Assistant, apply the offset, and ask Home Assistant again when it is time.
// Ask only with Wi-Fi: in eco mode and on the server screen eco_wake() asks in the session.
// Return true if the request goes now.
static bool tz_step(void)
{
    static unsigned seen;
    static time_t asked;
    const time_t now = time(NULL);
    hadata_tz_t tz;
    const unsigned count = hadata_get_tz(&tz);
    if (count && count != seen) {
        seen = count;
        save_tz_field("tz_off", s_settings.tz_off, sizeof(s_settings.tz_off), tz.offset);
        save_tz_field("tz_next", s_settings.tz_next, sizeof(s_settings.tz_next), (long long)tz.next);
        save_tz_field("tz_noff", s_settings.tz_noff, sizeof(s_settings.tz_noff), tz.next_offset);
        save_tz_field("tz_chk", s_settings.tz_chk, sizeof(s_settings.tz_chk), (long long)now);
    }
    apply_tz();
    if (now < VALID_TIME || !s_settings.ha_url[0] || !s_settings.ha_token[0] || !net_connected()) {
        return false;
    }
    const time_t checked = (time_t)atoll(s_settings.tz_chk);
    const time_t next = (time_t)atoll(s_settings.tz_next);
    const bool due = !tz_known() || now - checked >= TZ_CHECK_S || (next && now >= next) ||
                     (next && next - now <= TZ_WEEK_S && checked < next - TZ_WEEK_S);
    if (!due || now - asked < TZ_RETRY_S) {
        return false;
    }
    asked = now;
    ESP_LOGI(TAG, "Time zone: ask Home Assistant");
    hadata_request_tz();
    return true;
}

static unsigned s_tz_before;    // the count of the time zone answers before the request

static bool tz_read(void)
{
    hadata_tz_t tz;
    return hadata_get_tz(&tz) != s_tz_before;
}

// Return the milliseconds to the start of the next minute, plus a small margin.
static uint32_t ms_to_next_minute(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < VALID_TIME) {
        return 60000;
    }
    const uint32_t into = (uint32_t)(tv.tv_sec % 60) * 1000 + (uint32_t)(tv.tv_usec / 1000);
    return 60000 - into + 50;
}

// Status text for the settings page.
static void web_status(char *buf, size_t len)
{
    const ui_state_t *s = &s_last;
    if (!s->version) {
        snprintf(buf, len, "%s\n", tr(T_STATUS_STARTING));
        return;
    }
    size_t n = 0;
// Add a line. Stop when the buffer is full.
#define ADD(...)                                                   \
    do {                                                           \
        if (n < len) {                                             \
            const int w_ = snprintf(buf + n, len - n, __VA_ARGS__); \
            n += w_ > 0 ? (size_t)w_ : 0;                          \
        }                                                          \
    } while (0)
// Add a line of a language file: the text and a line end.
#define LINE(...)          \
    do {                   \
        ADD(__VA_ARGS__);  \
        ADD("\n");         \
    } while (0)

    const long long up = esp_timer_get_time() / 1000000;
    LINE(tr(T_STATUS_VERSION), s->version, up / 3600, up / 60 % 60);
    const esp_partition_t *running = esp_ota_get_running_partition();
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    LINE(tr(T_STATUS_FW_ROLLBACK), running ? running->label : "?");
#else
    LINE(tr(T_STATUS_FW_NO_ROLLBACK),
        running ? running->label : "?");
#endif
#if CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
    LINE("%s", tr(T_STATUS_SIGNED));
#else
    LINE("%s", tr(T_STATUS_UNSIGNED));
#endif
    if (!eco_on()) {
        LINE("%s", tr(T_STATUS_ECO_OFF));
    } else {
        LINE(tr(T_STATUS_ECO_ON), eco_minutes());
    }
    LINE(tr(T_STATUS_SOUND), tr(alert_sound_on() ? T_STATUS_ON : T_STATUS_OFF), volume());
    if (s->now) {
        struct tm t;
        localtime_r(&s->now, &t);
        LINE(tr(T_STATUS_TIME), t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
            t.tm_hour, t.tm_min, s->time_source ? s->time_source : tr(T_STATUS_UNKNOWN));
    } else {
        LINE("%s", tr(T_STATUS_TIME_NOT_SET));
    }
    const ha_data_t *h = &s_last_ha;
    if (!h->configured) {
        LINE(tr(T_STATUS_HA), h->error[0] ? h->error : tr(T_STATUS_NOT_CONFIGURED));
    } else if (h->updated) {
        struct tm t;
        localtime_r(&h->updated, &t);
        if (h->ok) {
            LINE(tr(T_STATUS_HA_DATA), t.tm_hour, t.tm_min);
        } else {
            LINE(tr(T_STATUS_HA_DATA_ERROR), t.tm_hour, t.tm_min, h->error);
        }
    } else if (h->error[0]) {
        LINE(tr(T_STATUS_HA_NO_DATA_ERROR), h->error);
    } else {
        LINE("%s", tr(T_STATUS_HA_NO_DATA));
    }
    LINE(tr(T_STATUS_FORECAST), h->forecast_valid ? h->forecast_text : tr(T_STATUS_NONE));
    if (h->outdoor_meteo) {
        char now_text[48];
        struct tm at;
        localtime_r(&h->outdoor_at, &at);
        snprintf(now_text, sizeof(now_text), "%+.0f°%s", h->temperature.value, "");
        if (h->humidity.valid) {
            const size_t used = strlen(now_text);
            snprintf(now_text + used, sizeof(now_text) - used, ", %.0f %%", h->humidity.value);
        }
        LINE(tr(T_STATUS_WEATHER_NOW), now_text, at.tm_hour, at.tm_min);
    }
    if (h->alert_ubilling) {
        if (h->alert_at) {
            struct tm at;
            localtime_r(&h->alert_at, &at);
            LINE(tr(T_STATUS_ALERTS_UB), region_name(), at.tm_hour, at.tm_min);
        } else {
            LINE(tr(T_STATUS_ALERTS_UB_NONE), region_name());
        }
    }
    // The power source, and where the DTEK state comes from (to find a wrong state).
    char power[96];
    ui_power_text(s, power, sizeof(power));
    ADD("%s\n", power);
    struct tm t;
    switch (h->dtek_source) {
    case HADATA_DTEK_HA:
        LINE(tr(T_STATUS_DTEK_HA), tr(h->dtek == 1 ? T_STATUS_YES : T_STATUS_NONE));
        break;
    case HADATA_DTEK_HC:
        localtime_r(&h->hc_at, &t);
        LINE(tr(T_STATUS_DTEK_HC), tr(h->dtek == 1 ? T_STATUS_YES : T_STATUS_NONE), h->hc_state,
            t.tm_hour, t.tm_min);
        break;
    case HADATA_DTEK_PING:
        localtime_r(&h->ping_at, &t);
        LINE(tr(T_STATUS_DTEK_PING), tr(h->dtek == 1 ? T_STATUS_YES : T_STATUS_NONE), s_settings.ping_host,
            t.tm_hour, t.tm_min, tr(h->dtek == 1 ? T_STATUS_ANSWERS : T_STATUS_NO_ANSWER));
        break;
    default:
        LINE("%s", tr(T_STATUS_DTEK_NONE));
        break;
    }
    LINE(tr(T_STATUS_UPS), tr(h->flat_powered == 1 ? T_STATUS_UPS_INPUT
                              : (h->flat_powered == 0 ? T_STATUS_UPS_BATTERY : T_STATUS_UPS_NONE)));
    if (s->wifi_connected) {
        LINE(tr(T_STATUS_WIFI), net_ssid(), s->ip, s->wifi_rssi);
    } else {
        LINE(tr(T_STATUS_WIFI_OFF), s->wifi_reason);
    }
    if (s->ap_active) {
        LINE(tr(T_STATUS_AP), s->ap_ssid, s->ap_ip);
    }
    if (!s->mqtt_enabled) {
        LINE("%s", tr(T_STATUS_MQTT_OFF));
    } else {
        LINE(tr(T_STATUS_MQTT), s->mqtt_connected ? tr(T_STATUS_CONNECTED) : (s->mqtt_error[0] ? s->mqtt_error : tr(T_STATUS_NO_LINK)));
    }
    if (s->screen == UI_SCREEN_SERVER) {
        char image[240];
        srvimg_status(image, sizeof(image));
        LINE(tr(T_STATUS_IMAGE), image[0] ? image : tr(T_STATUS_NOT_YET));
    }
    if (s->room_valid) {
        LINE(tr(T_STATUS_ROOM), (double)s->room_temperature, (double)s->room_humidity);
    }
    if (s->battery_mv > 0) {
        if (s->battery_percent >= 0 && s->usb_power) {
            LINE(tr(T_STATUS_BATTERY_CHARGING),
                s->battery_mv / 1000.0, s->battery_percent);
        } else if (s->battery_percent >= 0) {
            LINE(tr(T_STATUS_BATTERY), s->battery_mv / 1000.0, s->battery_percent);
        } else {
            LINE(tr(T_STATUS_BATTERY_USB),
                s->battery_mv / 1000.0);
        }
    }
    if (s_light_sleep >= 0) {
        LINE(tr(T_STATUS_SLEEP), tr(s_light_sleep ? T_STATUS_SLEEP_YES : T_STATUS_SLEEP_USB));
    }
    if (tz_known()) {
        const time_t next = (time_t)atoll(s_settings.tz_next);
        const int offset = tz_offset_at(time(NULL));
        ADD(tr(T_STATUS_TZ), offset < 0 ? '-' : '+', abs(offset) / 3600,
            abs(offset) / 60 % 60);
        if (next && time(NULL) < next) {
            struct tm tm;
            localtime_r(&next, &tm);
            const int after = atoi(s_settings.tz_noff);
            ADD(tr(T_STATUS_TZ_NEXT), after < 0 ? '-' : '+', abs(after) / 3600,
                abs(after) / 60 % 60, tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
        }
        ADD("\n");
    } else {
        LINE(tr(T_STATUS_TZ_FIRMWARE), CONFIG_RLCD_TZ);
    }
    if (s_settings.scr_back[0]) {
        LINE(tr(T_STATUS_LOW_BATTERY),
            ui_screen_name(ui_screen_from_id(s_settings.scr_back)));
    }
#undef LINE
#undef ADD
}

static void show(const ui_state_t *state)
{
    ui_render(state);
    const esp_err_t err = st7305_show(gfx_canvas());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot update the panel: %s", esp_err_to_name(err));
    } else {
        s_screens_shown++;
    }
}

static void factory_reset(ui_state_t *state)
{
    ESP_LOGW(TAG, "BOOT was held for 10 s: erase the settings and restart");
    state->message = tr(T_MSG_RESET);
    show(state);
    settings_reset();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void next_screen(ui_screen_t *screen)
{
    // KEY shows the next screen. The note screen is in the cycle only when it has a text.
    // The clock screen turns Wi-Fi and MQTT off. Keep the choice for the next start.
    forget_screen_back();
    do {
        *screen = (ui_screen_t)((*screen + 1) % UI_SCREEN_COUNT);
    } while ((*screen == UI_SCREEN_NOTE && s_settings.note[0] == '\0') || !screen_available(*screen));
    strlcpy(s_settings.screen, ui_screen_id(*screen), sizeof(s_settings.screen));
    settings_save_field("screen", s_settings.screen);
    ESP_LOGI(TAG, "KEY: screen \"%s\"", s_settings.screen);
    publish_screen(*screen, true);
}

// Low battery: below LOW_BATTERY_CLOCK % the device goes to the clock screen (the autonomous mode) and keeps
// the screen before it in the settings (scr_back). With USB power (USB data or a charge) it goes back to that
// screen. Under LOW_BATTERY_AGAIN % the switch comes only once: KEY can leave the clock. Return true if the
// screen changed.
#define LOW_BATTERY_CLOCK 10
#define LOW_BATTERY_AGAIN 15

static void set_screen(ui_screen_t *screen, ui_screen_t next)
{
    *screen = next;
    strlcpy(s_settings.screen, ui_screen_id(next), sizeof(s_settings.screen));
    settings_save_field("screen", s_settings.screen);
    publish_screen(next, true);
}

static bool battery_guard(ui_screen_t *screen, const ui_state_t *state)
{
    static bool done;   // the switch to the clock came for this discharge
    if (state->usb_power) {
        done = false;
        s_extreme = false;
        if (!s_settings.scr_back[0]) {
            return false;
        }
        const ui_screen_t back = ui_screen_from_id(s_settings.scr_back);
        s_settings.scr_back[0] = '\0';
        settings_save_field("scr_back", s_settings.scr_back);
        if (back == *screen) {
            return false;
        }
        ESP_LOGI(TAG, "USB power: back to the screen \"%s\"", ui_screen_id(back));
        set_screen(screen, back);
        return true;
    }
    if (state->battery_percent < 0) {
        return false;
    }
    if (state->battery_percent >= LOW_BATTERY_AGAIN) {
        done = false;
        return false;
    }
    bool changed = false;
    if (state->battery_percent < LOW_BATTERY_CLOCK && !done && !s_settings.scr_back[0]) {
        done = true;
        if (*screen != UI_SCREEN_CLOCK) {
            strlcpy(s_settings.scr_back, ui_screen_id(*screen), sizeof(s_settings.scr_back));
            settings_save_field("scr_back", s_settings.scr_back);
            ESP_LOGW(TAG, "Battery %d %%: the clock screen until USB power", state->battery_percent);
            set_screen(screen, UI_SCREEN_CLOCK);
            changed = true;
        }
    }
    // Below EXTREME_BATTERY %: the days-in-year screen. It does not change the kept screen.
    if (state->battery_percent < EXTREME_BATTERY && !s_extreme) {
        s_extreme = true;
        changed = true;
    }
    return changed;
}

// Collect the data and draw the screen.
static void update(ui_screen_t screen, const char *time_source, ui_state_t *state, ha_data_t *ha)
{
    // The screen from trmnl-server has the data: then the device does not read Home Assistant itself.
    const bool server = screen == UI_SCREEN_SERVER && !s_extreme;
    srvimg_set_active(server);
    hadata_pause(server || s_extreme);
    const bool eco = eco_active();
    // In eco mode and on the server screen the data comes only at the updates: keep it (also the DTEK state)
    // to the next one.
    const bool slow = eco || server;
    // "ЕКО" only in the first minute after the switch: then it tells nothing new.
    static bool was_slow;
    static int64_t slow_since;
    if (slow && !was_slow) {
        slow_since = esp_timer_get_time();
    }
    was_slow = slow;
    const int slow_age = (server ? server_minutes() : eco_minutes()) * 60 + HA_MAX_AGE_S;
    hadata_set_max_age(slow ? slow_age : HA_MAX_AGE_S, slow ? slow_age : 0);
    hadata_get(ha);
    *state = (ui_state_t){
        .screen = s_extreme ? UI_SCREEN_DAYS : screen,
        .ha = ha,
        .ha_max_age_s = slow ? slow_age : HA_MAX_AGE_S,
        .eco = slow,    // Wi-Fi is on only at the updates
        .eco_label = slow && esp_timer_get_time() - slow_since < MODE_LABEL_US,
        .power_queue = s_settings.pw_queue,
        .note = s_settings.note,
        .time_source = time_source,
        .wifi_configured = s_settings.wifi_ssid[0] || s_settings.wifi2_ssid[0],
        .wifi_connected = net_connected(),
        .wifi_rssi = net_rssi(),
        .wifi_reason = net_last_reason(),
        .ip = s_last_ip,
        .ap_active = net_ap_active(),
        .ap_ssid = net_ap_ssid(),
        .ap_password = net_ap_password(),
        .ap_ip = net_ap_ip(),
        .mqtt_enabled = hass_enabled(),
        .mqtt_connected = hass_connected(),
        .mqtt_error = hass_last_error(),
        .version = full_version(),
        .message = web_ota_active() ? tr(T_MSG_UPDATING) : NULL,
    };
    const time_t now = time(NULL);
    state->now = now >= VALID_TIME ? now : 0;
    read_battery(state);
    apply_power_mode(state->usb_power);
    if (server) {
        static uint8_t *image;  // SRVIMG_BYTES in PSRAM
        if (!image) {
            image = heap_caps_malloc(SRVIMG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        time_t at = 0;
        if (image && srvimg_copy(image, &at)) {
            state->server_image = image;
            state->server_image_at = at;
        }
    }
    net_ip(s_last_ip, sizeof(s_last_ip));

    if (s_have_shtc3) {
        float t, h;
        if (shtc3_read(&t, &h) == ESP_OK) {
            state->room_valid = true;
            state->room_temperature = t + CONFIG_RLCD_ROOM_TEMP_OFFSET_X10 / 10.0f;
            state->room_humidity = h;
        }
    }

    s_last = *state;
    s_last_ha = *ha;
    s_last.ha = &s_last_ha;
    show(state);
}

static void publish(const ui_state_t *state)
{
    const hass_state_t mqtt = {
        .room_valid = state->room_valid,
        .room_temperature = state->room_temperature,
        .room_humidity = state->room_humidity,
        .battery_percent = state->battery_percent,
        .battery_mv = state->battery_mv,
        .usb_power = state->usb_power,
        .rssi = state->wifi_rssi,
    };
    hass_publish(&mqtt);
}

// Wait up to ms for a condition. Notifications (network, data, buttons) wake the wait. A button event stops
// the wait: the main loop takes it at once (KEY must not wait for the network). Return true if the condition
// is true.
static bool wait_for(bool (*done)(void), int ms)
{
    const int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (!done()) {
        if (esp_timer_get_time() >= until || board_peek_buttons() != 0) {
            return false;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }
    return true;
}

static unsigned s_reads_before;

static bool data_read(void)
{
    return hadata_read_count() != s_reads_before;
}

static bool image_read(void)
{
    return srvimg_read_count() != s_reads_before;
}

// A button event stops the session: the main loop takes it now. Wi-Fi and MQTT go on with the connection.
static bool session_stopped(void)
{
    if (board_peek_buttons() == 0) {
        return false;
    }
    ESP_LOGI(TAG, "A button: stop the session, the main loop takes the button");
    return true;
}

// Turn Wi-Fi and MQTT on, wait for the connection and read the data. Without Wi-Fi the device goes on
// with the last data. A button stops the waits (see session_stopped()).
static void eco_wake(void)
{
    net_resume();
    const bool wifi = wait_for(net_connected, ECO_WIFI_WAIT_MS);
    // MQTT after Wi-Fi: a first try without a network waits 10 s (reconnect_timeout_ms) for the next one, and
    // a session ends before it. Without Wi-Fi start MQTT too: it tries again by itself.
    hass_resume();
    if (session_stopped()) {
        return;
    }
    if (!wifi) {
        ESP_LOGW(TAG, "No Wi-Fi: keep the last data, try again at the next update");
        return;
    }
    // The time zone request goes now, in parallel with the other requests: between the sessions Wi-Fi is off.
    hadata_tz_t tz;
    s_tz_before = hadata_get_tz(&tz);
    const bool tz_asked = tz_step();
    if (srvimg_active()) {
        // The "З сервера" screen: the image from trmnl-server instead of the Home Assistant data.
        // Right after the connection a request can fail: try again before the Home Assistant data.
        for (int attempt = 1; attempt <= IMAGE_TRIES; attempt++) {
            s_reads_before = srvimg_read_count();
            srvimg_refresh();
            wait_for(image_read, ECO_DATA_WAIT_MS);
            if (srvimg_ok()) {
                break;
            }
            if (session_stopped()) {
                return;
            }
            ESP_LOGW(TAG, "No image from trmnl-server, try %d of %d", attempt, IMAGE_TRIES);
            if (attempt < IMAGE_TRIES) {
                vTaskDelay(pdMS_TO_TICKS(IMAGE_RETRY_MS));
            }
        }
        if (!srvimg_ok()) {
            // trmnl-server does not answer: read Home Assistant now. Without a fresh image the screen
            // draws the quadrants from this data. update() stops the requests again.
            ESP_LOGW(TAG, "No image from trmnl-server: read Home Assistant");
            hadata_pause(false);
            s_reads_before = hadata_read_count();
            hadata_refresh();
            wait_for(data_read, ECO_DATA_WAIT_MS);
        }
    } else {
        s_reads_before = hadata_read_count();
        hadata_refresh();
        wait_for(data_read, ECO_DATA_WAIT_MS);
    }
    // The data goes out after the connection; the retained commands come right after it.
    if (hass_enabled()) {
        wait_for(hass_connected, 5000);
    }
    if (tz_asked) {
        wait_for(tz_read, TZ_WAIT_MS);
    }
    apply_requests();
}

// The KEY, BOOT 10 s buttons in eco mode. Return true if the main loop must update now (BOOT, or KEY
// to the clock or the server screen: they have their own cycles).
static bool eco_buttons(uint32_t buttons, ui_screen_t *screen, ui_state_t *state)
{
    if (buttons & BOARD_BUTTON_KEY) {
        next_screen(screen);
        state->screen = *screen;
        const time_t t = time(NULL);
        state->now = t >= VALID_TIME ? t : 0;
        show(state);
        if (*screen == UI_SCREEN_CLOCK || *screen == UI_SCREEN_SERVER) {
            return true;
        }
    }
    if (buttons & BOARD_BUTTON_BOOT_10S) {
        factory_reset(state);
    }
    if (buttons & BOARD_BUTTON_BOOT) {
        // Update now, and keep Wi-Fi on for some time: then the settings page opens.
        s_awake_until = esp_timer_get_time() + ECO_AWAKE_MS * 1000LL;
    }
    return (buttons & (BOARD_BUTTON_BOOT | BOARD_BUTTON_BOOT_3S)) != 0;
}

// Eco mode: turn Wi-Fi and MQTT off and sleep to the next update. KEY shows the next screen with the
// last data (Wi-Fi stays off); BOOT updates now; BOOT 3 s starts the access point. After the sleep,
// turn Wi-Fi on and wait for the data. Return the buttons for the main loop.
static uint32_t eco_cycle(ui_screen_t *screen, ui_state_t *state, time_t *next)
{
    time_t now = time(NULL);
    if (*next == 0) {
        // The first cycle (after the start or after the normal mode): get the data and the commands first.
        *next = now + (time_t)eco_minutes() * 60;
        if (!net_connected() || (hass_enabled() && !hass_connected())) {
            eco_wake();
            return 0;
        }
    }

    // Give MQTT time to send the data and to get the retained commands. After BOOT and while somebody
    // uses the settings page, stay awake longer, and update the screen each minute.
    const int64_t start = esp_timer_get_time();
    const int64_t until = start + ECO_MQTT_WAIT_MS * 1000LL;
    const int64_t minute_end = start + (int64_t)ms_to_next_minute() * 1000;
    for (;;) {
        const int64_t t = esp_timer_get_time();
        if (t >= until && !keep_awake()) {
            break;
        }
        if (t >= minute_end) {
            return 0;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        apply_requests();
        // A screen button on the page: draw it now, the sleep comes in the next cycle.
        if (!eco_active() || apply_show_request(screen)) {
            return 0;
        }
        const uint32_t buttons = board_take_buttons();
        if (eco_buttons(buttons, screen, state)) {
            return buttons & BOARD_BUTTON_BOOT_3S;
        }
    }

    now = time(NULL);
    if (*next <= now) {
        *next = now + (time_t)eco_minutes() * 60;
    }
    // A sound (for example, the sound check) plays to its end before the sleep.
    while (audio_busy()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Eco mode: Wi-Fi off, the next update in %lld s", (long long)(*next - now));
    hass_suspend();
    net_suspend();

    uint32_t result = 0;
    for (;;) {
        now = time(NULL);
        if (now >= *next) {
            break;
        }
        const int64_t left_us = (int64_t)(*next - now) * 1000000;
        if (board_peek_buttons() != 0) {
            // A button event waits: take it now.
        } else if (!may_sleep()) {
            // USB: wait without sleep, at most to the next minute (see may_sleep()).
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left_us / 1000 > 60000 ? 60000 : left_us / 1000));
        } else {
            board_light_sleep(left_us);
        }
        // A pressed button gives its event at the release.
        uint32_t buttons = board_take_buttons();
        for (int waited = 0; !buttons && board_button_pressed() && waited < ECO_HOLD_WAIT_MS; waited += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
            buttons = board_take_buttons();
        }
        if (eco_buttons(buttons, screen, state)) {
            result = buttons & BOARD_BUTTON_BOOT_3S;
            break;
        }
    }

    *next = time(NULL) + (time_t)eco_minutes() * 60;
    if ((*screen == UI_SCREEN_CLOCK || *screen == UI_SCREEN_SERVER) && !(result & BOARD_BUTTON_BOOT_3S)) {
        // KEY showed the clock or the server screen: Wi-Fi and MQTT are off already. The clock keeps them off;
        // the server screen turns them on in its cycle.
        s_offline = true;
        return result;
    }
    // Wake up: Wi-Fi, MQTT, then the data.
    eco_wake();
    return result;
}

/* Clock and server screen: Wi-Fi and MQTT off */

// Turn Wi-Fi and MQTT off for the clock or the server screen.
static void offline_enter(const char *who)
{
    // A sound (for example, the sound check) plays to its end first.
    while (audio_busy()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "%s: Wi-Fi and MQTT off", who);
    hass_suspend();
    net_suspend();
    s_offline = true;
}

// Leave the clock or the server screen: turn Wi-Fi and MQTT on, then wait for the connection and the data
// (as after an eco sleep).
static void offline_leave(void)
{
    s_offline = false;
    ESP_LOGI(TAG, "Wi-Fi and MQTT on");
    eco_wake();
}

// Sleep for ms or to a button. With USB, wait without sleep (see may_sleep()).
// Return the buttons. A held button gives its event at the release.
static uint32_t offline_wait(uint32_t ms)
{
    if (board_peek_buttons() != 0) {
        // A button event waits (for example, it stopped a session): take it now.
    } else if (!may_sleep()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
    } else {
        board_light_sleep((int64_t)ms * 1000);
    }
    uint32_t buttons = board_take_buttons();
    for (int waited = 0; !buttons && board_button_pressed() && waited < ECO_HOLD_WAIT_MS; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        buttons = board_take_buttons();
    }
    return buttons;
}

// The buttons of the clock and the server screen. KEY shows the next screen (the main loop then turns Wi-Fi
// and MQTT on, if the new screen needs them); BOOT 3 s turns Wi-Fi on and starts the access point (the screen
// waits while it is on); BOOT 10 s erases the settings. Return the buttons: BOOT is for the caller.
static uint32_t offline_buttons(uint32_t buttons, ui_screen_t *screen, ui_state_t *state)
{
    if (buttons & BOARD_BUTTON_KEY) {
        next_screen(screen);
    }
    if (buttons & BOARD_BUTTON_BOOT_10S) {
        factory_reset(state);
    }
    if (buttons & BOARD_BUTTON_BOOT_3S) {
        ESP_LOGI(TAG, "BOOT was held for 3 s: Wi-Fi on, start the setup access point");
        s_offline = false;
        net_resume();
        hass_resume();
        net_request_ap();
        wait_for(net_ap_active, 5000);
        buttons &= ~BOARD_BUTTON_BOOT_3S;
    }
    return buttons;
}

// The clock screen: the autonomous mode. Wi-Fi and MQTT are off; the device sleeps to the next minute and draws
// the clock again. No Home Assistant data, no alert sound, no NTP: the time comes from the system clock and
// the clock chip. BOOT draws the clock now and keeps the chip awake for 30 s (USB). Return the buttons for
// the main loop.
static uint32_t clock_cycle(ui_screen_t *screen, ui_state_t *state)
{
    if (!s_offline) {
        offline_enter("Clock");
    }
    const uint32_t buttons = offline_buttons(offline_wait(ms_to_next_minute()), screen, state);
    if (buttons & BOARD_BUTTON_BOOT) {
        // Draw the clock now, and do not sleep for some time: then a computer on USB sees the device.
        s_usb_window_until = esp_timer_get_time() + USB_WINDOW_MS * 1000LL;
    }
    return buttons;
}

// Return the milliseconds to the next local midnight, plus a small margin. Without a valid time: one hour.
static uint32_t ms_to_midnight(void)
{
    const time_t now = time(NULL);
    if (now < VALID_TIME) {
        return 60 * 60 * 1000;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    const int left_s = 24 * 3600 - (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
    return (uint32_t)left_s * 1000 + 500;
}

// The extreme power saving below EXTREME_BATTERY %. Wi-Fi and MQTT are off. The screen shows the date and
// the days of the year (as the DaysInYear screen of trmnl-server) and "Потрібно зарядити", and changes once
// a day, at midnight. Each 10 minutes the device reads only the battery; with USB power the mode ends and the
// function returns. The buttons do not change the screen; BOOT keeps the chip awake for 30 s (then a computer
// on USB sees the device), BOOT 10 s erases the settings.
static void extreme_mode(ui_screen_t *screen, const char *time_source, ui_state_t *state, ha_data_t *ha)
{
    if (!s_offline) {
        offline_enter("Extreme power saving");
    }
    ESP_LOGW(TAG, "Battery under %d %%: extreme power saving", EXTREME_BATTERY);
    int day = -1;
    for (;;) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_yday != day) {
            update(*screen, time_source, state, ha);   // the days screen (s_extreme)
            day = tm.tm_yday;
        } else {
            read_battery(state);
        }
        if (state->usb_power) {
            s_extreme = false;
            ESP_LOGI(TAG, "USB power: the extreme power saving ends");
            return;
        }
        const uint32_t midnight = ms_to_midnight();
        const uint32_t buttons = offline_wait(midnight < EXTREME_CHECK_MS ? midnight : EXTREME_CHECK_MS);
        if (buttons & BOARD_BUTTON_BOOT_10S) {
            factory_reset(state);
        }
        if (buttons & BOARD_BUTTON_BOOT) {
            s_usb_window_until = esp_timer_get_time() + USB_WINDOW_MS * 1000LL;
        }
    }
}

// The buttons of the server screen. BOOT turns Wi-Fi and MQTT on now and keeps them on for some time:
// then the settings page opens.
static uint32_t server_buttons(uint32_t buttons, ui_screen_t *screen, ui_state_t *state, time_t *next)
{
    buttons = offline_buttons(buttons, screen, state);
    if (buttons & BOARD_BUTTON_BOOT) {
        *next = 0;
        s_awake_until = esp_timer_get_time() + ECO_AWAKE_MS * 1000LL;
        s_usb_window_until = esp_timer_get_time() + USB_WINDOW_MS * 1000LL;
    }
    return buttons;
}

// The "З сервера" screen: the semi-autonomous mode. Wi-Fi and MQTT are off, and the device draws the time in its
// quadrant each minute. Every 5 minutes it turns Wi-Fi and MQTT on and gets the commands and the image of
// trmnl-server (eco_wake()); the main loop then draws the screen and sends the room climate and the battery;
// then Wi-Fi and MQTT go off again. Without Wi-Fi, trmnl-server or MQTT the device goes on with the data that
// it has (see eco_wake() and draw_server() of ui.c). Return the buttons for the main loop.
static uint32_t server_cycle(ui_screen_t *screen, ui_state_t *state, time_t *next)
{
    const time_t now = time(NULL);
    if (*next == 0 || now >= *next) {
        *next = now + (time_t)server_minutes() * 60;
        if (s_offline) {
            s_offline = false;
            ESP_LOGI(TAG, "Server screen: Wi-Fi and MQTT on");
        }
        eco_wake();
        return 0;
    }
    if (!s_offline) {
        // The main loop sent the data: give MQTT time to send it. After BOOT and while somebody uses
        // the settings page, stay on, and draw the time each minute.
        const int64_t start = esp_timer_get_time();
        const int64_t until = start + ECO_MQTT_WAIT_MS * 1000LL;
        const int64_t minute_end = start + (int64_t)ms_to_next_minute() * 1000;
        for (;;) {
            const int64_t t = esp_timer_get_time();
            if (t >= until && !keep_awake()) {
                break;
            }
            if (t >= minute_end) {
                return 0;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            apply_requests();
            // A screen button on the page: draw it now.
            if (apply_show_request(screen) || !server_active(*screen)) {
                return 0;
            }
            const uint32_t buttons = board_take_buttons();
            if (buttons) {
                return server_buttons(buttons, screen, state, next);
            }
        }
        offline_enter("Server screen");
    }
    // Sleep to the next minute, or to the next communication if it comes first.
    uint32_t ms = ms_to_next_minute();
    const int64_t left_ms = ((int64_t)*next - (int64_t)time(NULL)) * 1000;
    if (left_ms < (int64_t)ms) {
        ms = left_ms > 0 ? (uint32_t)left_ms : 0;
    }
    return server_buttons(offline_wait(ms), screen, state, next);
}

void app_main(void)
{
    s_main_task = xTaskGetCurrentTaskHandle();
    const char *version = full_version();
    ESP_LOGI(TAG, "rlcd-dashboard %s", version);

    init_nvs();
    settings_load(&s_settings);
    texts_set_language(s_settings.lang);
    // A device that worked before the first-start wizard: the answers come from its settings.
    if (!s_settings.has_ha[0] && s_settings.wifi_ssid[0]) {
        strlcpy(s_settings.has_ha, s_settings.ha_url[0] ? "1" : "0", sizeof(s_settings.has_ha));
        strlcpy(s_settings.has_srv, s_settings.srv_url[0] ? "1" : "0", sizeof(s_settings.has_srv));
        settings_save_field("has_ha", s_settings.has_ha);
        settings_save_field("has_srv", s_settings.has_srv);
        ESP_LOGI(TAG, "Home Assistant: %s, trmnl-server: %s (from the settings)", s_settings.has_ha, s_settings.has_srv);
    }
    setenv("TZ", CONFIG_RLCD_TZ, 1);
    tzset();
    apply_tz();     // the kept answer of Home Assistant, if there is one

    ESP_ERROR_CHECK(board_init(s_main_task));

    const st7305_config_t lcd = {
        .host = SPI3_HOST,
        .sck = BOARD_LCD_SCK,
        .mosi = BOARD_LCD_MOSI,
        .cs = BOARD_LCD_CS,
        .dc = BOARD_LCD_DC,
        .rst = BOARD_LCD_RST,
        .clock_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(st7305_init(&lcd));

    s_have_shtc3 = shtc3_init(board_i2c_bus()) == ESP_OK;
    if (!s_have_shtc3) {
        ESP_LOGE(TAG, "No SHTC3: the room data is not available");
    }

    // Take the time from the hardware clock until SNTP gives the network time.
    const char *time_source = NULL;
    if (rtc_clock_init(board_i2c_bus()) == ESP_OK) {
        time_t utc;
        const esp_err_t err = rtc_clock_read(&utc);
        if (err == ESP_OK) {
            const struct timeval tv = { .tv_sec = utc };
            settimeofday(&tv, NULL);
            time_source = rtc_clock_name();
            ESP_LOGI(TAG, "The time comes from %s", time_source);
        } else {
            ESP_LOGW(TAG, "%s has no valid time (%s)", rtc_clock_name(), esp_err_to_name(err));
        }
    }

    if (audio_init(board_i2c_bus()) == ESP_OK) {
        audio_set_volume(volume());
    } else {
        ESP_LOGE(TAG, "No audio: the alert sound is not available");
    }

    net_set_auto_ap(!eco_on());
    net_start(&s_settings, CONFIG_RLCD_NTP_SERVER, wake_main_task);
    publish_controls();
    hass_start(&s_settings, version, wake_main_task, on_command);
    hadata_start(&s_settings, wake_main_task);
    srvimg_start(s_settings.srv_url, wake_main_task);
    const web_config_t web = {
        .settings = &s_settings,
        .version = version,
        .status = web_status,
        .on_change = wake_main_task,
        .on_show = on_show,
        .on_set = on_page_set,
        .on_sound = sound_check,
        .wait_applied = wait_page_applied,
    };
    web_start(&web);
    start_ota_check();

    ui_screen_t screen = ui_screen_from_id(s_settings.screen);
    if (screen == UI_SCREEN_NOTE && s_settings.note[0] == '\0') {
        screen = UI_SCREEN_DASHBOARD;
    }
    if (!screen_available(screen)) {
        // The kept screen needs a service that the user does not have: the room screen needs nothing.
        screen = UI_SCREEN_ROOM;
    }
    unsigned synced = 0;
    time_t eco_next = 0;
    time_t server_next = 0;
    ui_state_t state;
    ha_data_t ha;
    for (;;) {
        apply_requests();
        apply_show_request(&screen);
        tz_step();

        // After a network time synchronization, keep the time in the hardware clock.
        const unsigned count = net_sync_count();
        if (count != synced) {
            synced = count;
            time_source = "NTP";
            if (rtc_clock_kind() != RTC_CLOCK_NONE) {
                const esp_err_t err = rtc_clock_write(time(NULL));
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Cannot write the time to %s: %s", rtc_clock_name(), esp_err_to_name(err));
                }
            }
        }

        update(screen, time_source, &state, &ha);
        if (battery_guard(&screen, &state)) {
            continue;   // draw the new screen at once
        }
        if (s_extreme) {
            // It returns with USB power; then the next update shows the kept screen.
            extreme_mode(&screen, time_source, &state, &ha);
            continue;
        }
        const bool clock = clock_active(screen);
        const bool server = server_active(screen);
        if (!clock && !server && s_offline) {
            // KEY left the clock or the server screen: the new screen shows the last data. Turn Wi-Fi and MQTT
            // on, wait for the data, and draw the screen again.
            offline_leave();
            continue;
        }
        if (!clock) {
            alert_sound(&state, &ha);
            publish(&state);
        }

        uint32_t buttons;
        if (clock) {
            eco_next = 0;
            server_next = 0;
            buttons = clock_cycle(&screen, &state);
        } else if (server) {
            eco_next = 0;
            buttons = server_cycle(&screen, &state, &server_next);
        } else if (eco_active()) {
            server_next = 0;
            buttons = eco_cycle(&screen, &state, &eco_next);
        } else {
            eco_next = 0;
            server_next = 0;
            // Sleep to the next minute. A button or a network change wakes the task earlier. A button event
            // that waits (it stopped a session, and the wait took its notification) goes without the sleep.
            if (board_peek_buttons() == 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms_to_next_minute()));
            }
            buttons = board_take_buttons();
            if (buttons & BOARD_BUTTON_KEY) {
                next_screen(&screen);
            }
            if (buttons & BOARD_BUTTON_BOOT_10S) {
                factory_reset(&state);
            }
        }
        if (buttons & BOARD_BUTTON_BOOT_3S) {
            ESP_LOGI(TAG, "BOOT was held for 3 s: start the setup access point");
            net_request_ap();
        }
    }
}
