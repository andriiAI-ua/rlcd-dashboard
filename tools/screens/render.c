// Draw each screen with the code of the firmware (ui.c, gfx, fonts, texts) and sample data, and write the canvas
// as a PBM file: <out>/<language>/<screen>.pbm. tools/screens/render.py builds this program and makes the PNG
// files for the README.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "gfx.h"
#include "texts.h"
#include "ui.h"

#define STRIDE (GFX_WIDTH / 8)

static time_t local_time(int year, int month, int day, int hour, int minute)
{
    struct tm t = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = hour,
                    .tm_min = minute, .tm_isdst = -1 };
    return mktime(&t);
}

static void write_pbm(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.pbm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    // P4: 1 = black, rows from the top, MSB first: the format of the canvas.
    fprintf(f, "P4\n%d %d\n", GFX_WIDTH, GFX_HEIGHT);
    fwrite(gfx_canvas(), 1, STRIDE * GFX_HEIGHT, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: render <output folder>\n");
        return 2;
    }
    // The firmware draws the local time of Kyiv.
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
    const time_t now = local_time(2026, 11, 16, 14, 27);

    // A winter afternoon in Kyiv: a missile alert since 14:01, power from DTEK with an outage at 18:00.
    ha_data_t ha;
    memset(&ha, 0, sizeof(ha));
    ha.configured = true;
    ha.ok = true;
    ha.updated = now - 40;
    ha.temperature = (ha_sensor_t){ .valid = true, .value = 5.3, .has_change = true, .change = -1.8 };
    ha.humidity = (ha_sensor_t){ .valid = true, .value = 78, .has_change = true, .change = 6 };
    ha.aqi = (ha_sensor_t){ .valid = true, .value = 42, .has_change = true, .change = -9 };
    ha.voltage_valid = true;
    ha.voltage = 228;
    ha.alert_known = true;
    ha.missile = true;
    ha.alert_since = local_time(2026, 11, 16, 14, 1);
    ha.dtek = 1;
    ha.flat_powered = 1;
    ha.next_off = local_time(2026, 11, 16, 18, 0);
    ha.next_on = local_time(2026, 11, 16, 21, 30);
    ha.schedule_known = true;
    ha.forecast_valid = true;
    ha.forecast_has_temp = true;
    ha.forecast_temp = 7;
    // The city forecast of meteo.gov.ua: the night (03:00) and the day (15:00), temperature and FMI SmartSymbol.
    static const int DAYS[HADATA_DAYS][4] = {
        { 0, 104, 1, 7 }, { 0, 107, 1, 47 }, { -1, 154, 0, 4 }, { -4, 107, -3, 57 }, { -4, 107, -1, 7 },
        { -5, 104, -4, 6 }, { -3, 154, -2, 2 }, { -4, 106, -4, 54 }, { -8, 107, -7, 4 }, { -11, 101, -5, 1 },
    };
    for (int i = 0; i < HADATA_DAYS; i++) {
        ha.days[i].night = (hadata_point_t){ .valid = true, .temp = DAYS[i][0], .symbol = DAYS[i][1] };
        ha.days[i].day = (hadata_point_t){ .valid = true, .temp = DAYS[i][2], .symbol = DAYS[i][3] };
    }
    ha.days_at = now - 3600;

    static uint8_t server_image[STRIDE * GFX_HEIGHT];

    for (int lang = 0; lang < TEXTS_LANGUAGE_COUNT; lang++) {
        const char *code = texts_language_code(lang);
        texts_set_language(code);
        strcpy(ha.forecast_text, strcmp(code, "en") == 0 ? "cloudy with clearings, rain" : "хмарно з проясненнями, дощ");
        char dir[400];
        snprintf(dir, sizeof(dir), "%s/%s", argv[1], code);
        mkdir(argv[1], 0755);
        mkdir(dir, 0755);

        ui_state_t s;
        memset(&s, 0, sizeof(s));
        s.now = now;
        s.ha = &ha;
        s.power_queue = strcmp(code, "en") == 0 ? "group 1.1" : "черга 1.1";
        s.note = strcmp(code, "en") == 0 ? "Water in the jars on the balcony.\nThe keys are at the neighbour's."
                                         : "Вода в банках на балконі.\nКлючі — у сусідки.";
        s.room_valid = true;
        s.room_temperature = 21.4f;
        s.room_humidity = 44.0f;
        s.battery_mv = 3950;
        s.battery_percent = 76;
        s.wifi_configured = true;
        s.wifi_connected = true;
        s.forecast_city = true;

        static const struct {
            ui_screen_t screen;
            const char *name;
        } SCREENS[] = {
            { UI_SCREEN_DASHBOARD, "dashboard" }, { UI_SCREEN_ROOM, "room" }, { UI_SCREEN_NOTE, "note" },
            { UI_SCREEN_QUAD, "quad" }, { UI_SCREEN_QUAD5, "quad5" }, { UI_SCREEN_DAYS10, "days10" },
            { UI_SCREEN_CLOCK, "clock" },
        };
        for (size_t i = 0; i < sizeof(SCREENS) / sizeof(SCREENS[0]); i++) {
            s.screen = SCREENS[i].screen;
            ui_render(&s);
            write_pbm(dir, SCREENS[i].name);
        }

        // The server screen: the image of trmnl-server has the quadrants with the top left one empty. Here the
        // quadrants of the device stand for it; the device draws its time quadrant over it.
        s.screen = UI_SCREEN_QUAD;
        ui_render(&s);
        memcpy(server_image, gfx_canvas(), sizeof(server_image));
        for (int y = 0; y < GFX_HEIGHT / 2; y++) {
            memset(server_image + y * STRIDE, 0, STRIDE / 2);
        }
        s.screen = UI_SCREEN_SERVER;
        s.eco = true;
        s.server_image = server_image;
        s.server_image_at = now - 120;
        ui_render(&s);
        write_pbm(dir, "server");
        s.eco = false;
        s.server_image = NULL;

        // The extreme power saving below 2 %: the days of the year.
        s.screen = UI_SCREEN_DAYS;
        s.battery_percent = 1;
        s.battery_mv = 3320;
        ui_render(&s);
        write_pbm(dir, "days");
        s.battery_percent = 76;
        s.battery_mv = 3950;

        // The Wi-Fi setup with the access point and its QR code.
        s.screen = UI_SCREEN_DASHBOARD;
        s.ap_active = true;
        s.wifi_connected = false;
        s.ap_ssid = "RLCD-Setup-3F2A";
        s.ap_password = "rlcd3f2a";
        s.ap_ip = "10.0.0.1";
        ui_render(&s);
        write_pbm(dir, "setup");
        s.ap_active = false;
        s.wifi_connected = true;

        // A message over the screen: the firmware update.
        s.screen = UI_SCREEN_ROOM;
        s.message = tr(T_MSG_UPDATING);
        ui_render(&s);
        write_pbm(dir, "message");
        s.message = NULL;
    }
    return 0;
}
