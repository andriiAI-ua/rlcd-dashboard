// Settings in the NVS namespace "rlcd". Each field is one string key.
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NAMESPACE = "rlcd";

typedef struct {
    const char *key;     // NVS key, 15 characters maximum
    size_t offset;
    size_t size;
    const char *def;     // build value
} field_t;

#define FIELD(name, def) { #name, offsetof(settings_t, name), sizeof(((settings_t *)0)->name), def }

// An sdkconfig from an older version does not have the newer options yet.
#ifndef CONFIG_RLCD_HC_KEY
#define CONFIG_RLCD_HC_KEY ""
#endif
#ifndef CONFIG_RLCD_HC_CHECK
#define CONFIG_RLCD_HC_CHECK ""
#endif
#ifndef CONFIG_RLCD_ECO_MINUTES
#define CONFIG_RLCD_ECO_MINUTES "30"
#endif

static const field_t FIELDS[] = {
    FIELD(wifi_ssid, CONFIG_RLCD_WIFI_SSID),
    FIELD(wifi_pass, CONFIG_RLCD_WIFI_PASSWORD),
    FIELD(wifi2_ssid, CONFIG_RLCD_WIFI2_SSID),
    FIELD(wifi2_pass, CONFIG_RLCD_WIFI2_PASSWORD),
    FIELD(dns1, CONFIG_RLCD_DNS1),
    FIELD(dns2, CONFIG_RLCD_DNS2),
    FIELD(mqtt_uri, CONFIG_RLCD_MQTT_URI),
    FIELD(mqtt_user, CONFIG_RLCD_MQTT_USER),
    FIELD(mqtt_pass, CONFIG_RLCD_MQTT_PASSWORD),
    FIELD(mqtt_prefix, CONFIG_RLCD_MQTT_PREFIX),
    FIELD(web_pass, CONFIG_RLCD_WEB_PASSWORD),
    FIELD(ap_pass, ""),
    FIELD(ha_url, CONFIG_RLCD_HA_URL),
    FIELD(ha_token, CONFIG_RLCD_HA_TOKEN),
    FIELD(e_temp, CONFIG_RLCD_E_TEMP),
    FIELD(e_hum, CONFIG_RLCD_E_HUM),
    FIELD(e_aqi, CONFIG_RLCD_E_AQI),
    FIELD(e_volt, CONFIG_RLCD_E_VOLT),
    FIELD(e_ups, CONFIG_RLCD_E_UPS),
    FIELD(e_drone, CONFIG_RLCD_E_DRONE),
    FIELD(e_missile, CONFIG_RLCD_E_MISSILE),
    FIELD(e_off, CONFIG_RLCD_E_OFF),
    FIELD(e_on, CONFIG_RLCD_E_ON),
    FIELD(e_dtek, CONFIG_RLCD_E_DTEK),
    FIELD(hc_key, CONFIG_RLCD_HC_KEY),
    FIELD(hc_check, CONFIG_RLCD_HC_CHECK),
    FIELD(ping_host, ""),
    FIELD(pw_queue, CONFIG_RLCD_POWER_QUEUE),
    FIELD(meteo, CONFIG_RLCD_METEO_STATION),
    FIELD(alert_reg, CONFIG_RLCD_ALERT_REGION),
    FIELD(srv_url, CONFIG_RLCD_SERVER_URL),
    FIELD(srv_min, "5"),
    FIELD(screen, "dashboard"),
    FIELD(note, ""),
    FIELD(scr_back, ""),
    FIELD(eco, "0"),
    FIELD(eco_min, CONFIG_RLCD_ECO_MINUTES),
    FIELD(snd_alert, "1"),
    FIELD(snd_vol, "70"),
    FIELD(snd_missile, "siren"),
    FIELD(snd_drone, "warble"),
    FIELD(snd_clear, "chime"),
    FIELD(ota_url, CONFIG_RLCD_OTA_URL),
    FIELD(host, CONFIG_RLCD_HOST),
    FIELD(lang, CONFIG_RLCD_LANGUAGE),
    FIELD(has_ha, ""),
    FIELD(has_srv, ""),
    FIELD(tz_off, ""),
    FIELD(tz_next, ""),
    FIELD(tz_noff, ""),
    FIELD(tz_chk, ""),
};

#define FIELD_COUNT (sizeof(FIELDS) / sizeof(FIELDS[0]))

static char *field_ptr(settings_t *s, const field_t *f)
{
    return (char *)s + f->offset;
}

esp_err_t settings_load(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    nvs_handle_t h = 0;
    const esp_err_t open_err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (open_err != ESP_OK && open_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Cannot open NVS: %s. Use the build values.", esp_err_to_name(open_err));
    }

    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const field_t *f = &FIELDS[i];
        char *dst = field_ptr(s, f);
        size_t len = f->size;
        if (open_err != ESP_OK || nvs_get_str(h, f->key, dst, &len) != ESP_OK) {
            strlcpy(dst, f->def, f->size);
        }
    }
    if (open_err == ESP_OK) {
        nvs_close(h);
    }

    // The access point password is made once and kept, so that it does not change.
    if (s->ap_pass[0] == '\0') {
        snprintf(s->ap_pass, sizeof(s->ap_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
        nvs_handle_t w;
        if (nvs_open(NAMESPACE, NVS_READWRITE, &w) == ESP_OK) {
            nvs_set_str(w, "ap_pass", s->ap_pass);
            nvs_commit(w);
            nvs_close(w);
        }
    }
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &h), TAG, "open NVS");
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < FIELD_COUNT && err == ESP_OK; i++) {
        const field_t *f = &FIELDS[i];
        err = nvs_set_str(h, f->key, field_ptr((settings_t *)s, f));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "write NVS");
    ESP_LOGI(TAG, "Settings are saved");
    return ESP_OK;
}

esp_err_t settings_save_field(const char *key, const char *value)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &h), TAG, "open NVS");
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_reset(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &h), TAG, "open NVS");
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "Settings are erased");
    return err;
}

bool settings_screen_available(const settings_t *s, const char *id)
{
    if (strcmp(id, "dashboard") == 0 || strcmp(id, "quad") == 0) {
        return s->has_ha[0] != '0';
    }
    if (strcmp(id, "server") == 0) {
        return s->has_srv[0] != '0';
    }
    return true;
}
