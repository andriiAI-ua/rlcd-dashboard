// Screens, 400 x 300, black and white.
// The dashboard is the compact screen of trmnl-server (DashboardRenderer: BuildView and DrawCompact):
// the same texts, positions and font sizes, in the black and white palette (Mono).
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "gfx_fonts.h"
#include "qrcode.h"
#include "texts.h"
#include "time_util.h"

#define MARGIN              6
#define LOW_BATTERY_PERCENT 20
#define HA_FRESH_S          (5 * 60)   // older Home Assistant data is not shown

// The lists of the language files must fit the code.
_Static_assert(T_DATE_MONTHS_ITEMS == 12, "lang: date.months must have 12 items");
_Static_assert(T_DATE_WEEKDAYS_ITEMS == 7, "lang: date.weekdays must have 7 items (from Sunday)");
_Static_assert(T_SCREEN_NAMES_ITEMS == UI_SCREEN_COUNT, "lang: screen.names must have one item per screen");
_Static_assert(T_AQI_LEVELS_ITEMS == 6, "lang: aqi.levels must have 6 items");
_Static_assert(T_DATE_WEEKDAYS_TITLE_ITEMS == 7, "lang: date.weekdays_title must have 7 items (from Sunday)");
_Static_assert(T_DATE_WEEKDAYS_SHORT_ITEMS == 7, "lang: date.weekdays_short must have 7 items (from Sunday)");
_Static_assert(T_WEATHER_SKY_ITEMS == 6, "lang: weather.sky must have 6 items");
_Static_assert(T_WEATHER_DRIZZLE_ITEMS == 3, "lang: weather.drizzle must have 3 items");
_Static_assert(T_WEATHER_RAIN_ITEMS == 3, "lang: weather.rain must have 3 items");
_Static_assert(T_WEATHER_SLEET_ITEMS == 3, "lang: weather.sleet must have 3 items");
_Static_assert(T_WEATHER_SNOW_ITEMS == 3, "lang: weather.snow must have 3 items");

#define MONTH(t)   tra(T_DATE_MONTHS, (t)->tm_mon)
#define WEEKDAY(t) tra(T_DATE_WEEKDAYS, (t)->tm_wday)

const char *ui_screen_id(ui_screen_t screen)
{
    switch (screen) {
    case UI_SCREEN_ROOM:
        return "room";
    case UI_SCREEN_NOTE:
        return "note";
    case UI_SCREEN_QUAD:
        return "quad";
    case UI_SCREEN_QUAD5:
        return "quad5";
    case UI_SCREEN_DAYS10:
        return "days10";
    case UI_SCREEN_SERVER:
        return "server";
    case UI_SCREEN_CLOCK:
        return "clock";
    default:
        return "dashboard";
    }
}

ui_screen_t ui_screen_from_id(const char *id)
{
    if (id && strcmp(id, "room") == 0) {
        return UI_SCREEN_ROOM;
    }
    if (id && strcmp(id, "note") == 0) {
        return UI_SCREEN_NOTE;
    }
    if (id && strcmp(id, "quad") == 0) {
        return UI_SCREEN_QUAD;
    }
    if (id && strcmp(id, "quad5") == 0) {
        return UI_SCREEN_QUAD5;
    }
    if (id && strcmp(id, "days10") == 0) {
        return UI_SCREEN_DAYS10;
    }
    if (id && strcmp(id, "server") == 0) {
        return UI_SCREEN_SERVER;
    }
    if (id && strcmp(id, "clock") == 0) {
        return UI_SCREEN_CLOCK;
    }
    return UI_SCREEN_DASHBOARD;
}

// The names of the screens are in the order of ui_screen_t (screen.names of the language files).
const char *ui_screen_name(ui_screen_t screen)
{
    return tra(T_SCREEN_NAMES, screen < UI_SCREEN_COUNT ? (int)screen : (int)UI_SCREEN_DASHBOARD);
}

ui_screen_t ui_screen_from_name(const char *name)
{
    if (!name) {
        return UI_SCREEN_COUNT;
    }
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        if (strcmp(name, ui_screen_name((ui_screen_t)i)) == 0 || strcmp(name, ui_screen_id((ui_screen_t)i)) == 0) {
            return (ui_screen_t)i;
        }
    }
    return UI_SCREEN_COUNT;
}

/* Texts */

typedef struct {
    int dir;            // 1: up, -1: down, 0: no change
    char text[48];
    char bare[24];      // the value without the words ("1,2°"), or "no change" for no change
} trend_t;

// Format a positive number with a decimal comma.
static void format_abs(char *buf, size_t len, double value, int decimals)
{
    snprintf(buf, len, "%.*f", decimals, fabs(value));
    for (char *p = buf; *p; p++) {
        if (*p == '.') {
            *p = ',';
        }
    }
}

// "+5,3°", "−2,0°", "0,0°" (FormatTemperature).
static void format_temperature(char *buf, size_t len, double value)
{
    const double r = round(value * 10) / 10;
    char num[16];
    format_abs(num, sizeof(num), r, 1);
    snprintf(buf, len, "%s%s°", r > 0 ? "+" : (r < 0 ? "−" : ""), num);
}

// MakeTrend: "1,2° за добу", "без змін за добу". The change is already rounded.
static void make_trend(trend_t *t, double change, int decimals, const char *unit)
{
    if (change == 0) {
        t->dir = 0;
        strlcpy(t->text, tr(T_TREND_NO_CHANGE_DAY), sizeof(t->text));
        strlcpy(t->bare, tr(T_TREND_NO_CHANGE), sizeof(t->bare));
        return;
    }
    char num[16];
    format_abs(num, sizeof(num), change, decimals);
    t->dir = change > 0 ? 1 : -1;
    snprintf(t->bare, sizeof(t->bare), "%s%s", num, unit);
    snprintf(t->text, sizeof(t->text), tr(T_TREND_PER_DAY), t->bare);
}

// TrimDay: only the value, without "per day".
static void trim_day(trend_t *t)
{
    strlcpy(t->text, t->bare, sizeof(t->text));
}

// A lower AQI is cleaner air (AirQualityTrendOf). The bare text is the number only.
static void aqi_trend(trend_t *t, double change)
{
    if (change == 0) {
        t->dir = 0;
        strlcpy(t->text, tr(T_TREND_NO_CHANGE), sizeof(t->text));
        strlcpy(t->bare, t->text, sizeof(t->bare));
        return;
    }
    t->dir = change < 0 ? -1 : 1;
    snprintf(t->bare, sizeof(t->bare), "%.0f", fabs(change));
    snprintf(t->text, sizeof(t->text), tr(change < 0 ? T_AQI_CLEANER : T_AQI_DIRTIER), t->bare);
}

static const char *aqi_level(double aqi)
{
    const int level = aqi <= 50 ? 0 : aqi <= 100 ? 1 : aqi <= 150 ? 2 : aqi <= 200 ? 3 : aqi <= 300 ? 4 : 5;
    return tra(T_AQI_LEVELS, level);
}

// Days between the local dates of two times.
static int day_diff(time_t t, time_t now)
{
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    const time_t da = time_utc(a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, 0, 0, 0);
    const time_t db = time_utc(b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, 0, 0, 0);
    return (int)((da - db) / 86400);
}

static void hhmm(char *buf, size_t len, time_t t)
{
    struct tm l;
    localtime_r(&t, &l);
    snprintf(buf, len, "%02d:%02d", l.tm_hour, l.tm_min);
}

// "Вимкнення сьогодні", "Вимкнення завтра", "Вимкнення 14.11" (WithDay).
static void with_day(char *buf, size_t len, const char *label, time_t t, time_t now)
{
    const int days = day_diff(t, now);
    if (days == 0) {
        snprintf(buf, len, tr(T_DAY_TODAY), label);
    } else if (days == 1) {
        snprintf(buf, len, tr(T_DAY_TOMORROW), label);
    } else {
        struct tm l;
        localtime_r(&t, &l);
        snprintf(buf, len, "%s %02d.%02d", label, l.tm_mday, l.tm_mon + 1);
    }
}

// "до 18:30", "до завтра 06:00", "до 14.11 06:00" (Until).
static void until(char *buf, size_t len, time_t t, time_t now)
{
    char hm[8];
    hhmm(hm, sizeof(hm), t);
    const int days = day_diff(t, now);
    if (days == 0) {
        snprintf(buf, len, tr(T_UNTIL_TODAY), hm);
    } else if (days == 1) {
        snprintf(buf, len, tr(T_UNTIL_TOMORROW), hm);
    } else {
        struct tm l;
        localtime_r(&t, &l);
        snprintf(buf, len, tr(T_UNTIL_DATE), l.tm_mday, l.tm_mon + 1, hm);
    }
}

// "12:00–16:00" or "о 12:00" (OffWindow).
static void off_window(char *buf, size_t len, time_t off, time_t back)
{
    char a[8], b[8];
    hhmm(a, sizeof(a), off);
    if (back > off) {
        hhmm(b, sizeof(b), back);
        snprintf(buf, len, "%s–%s", a, b);
    } else {
        snprintf(buf, len, tr(T_AT_TIME), a);
    }
}

// Three states of the power in the flat.
typedef enum { POWER_DTEK, POWER_GENERATOR, POWER_NONE } power_kind_t;

typedef struct {
    power_kind_t kind;
    const char *status;     // "Світло від генератора"
    const char *source;     // "від генератора": under the title "Світло" this is enough
    char label[48];
    char value[48];
} power_view_t;

// The power state:
// - the UPS has no input power (it works from its battery): no power;
// - DTEK gives power: power from DTEK;
// - the UPS has input power without DTEK: power from the generator;
// - no UPS data (for example, the UPS battery is empty) and no DTEK power: no power.
// The DTEK state comes from hadata (the Home Assistant entity, healthchecks.io, the ping); when no source
// knows it, there is no DTEK power. The next change: PowerTexts of trmnl-server.
static void power_view(const ha_data_t *h, time_t now, power_view_t *v)
{
    if (h->flat_powered == 0) {
        v->kind = POWER_NONE, v->status = tr(T_POWER_NONE), v->source = tr(T_POWER_SRC_NONE);
    } else if (h->dtek == 1) {
        v->kind = POWER_DTEK, v->status = tr(T_POWER_DTEK), v->source = tr(T_POWER_SRC_DTEK);
    } else if (h->flat_powered == 1) {
        v->kind = POWER_GENERATOR, v->status = tr(T_POWER_GENERATOR), v->source = tr(T_POWER_SRC_GENERATOR);
    } else {
        v->kind = POWER_NONE, v->status = tr(T_POWER_NONE), v->source = tr(T_POWER_SRC_NONE);
    }

    // Svitlo.live gives only one of the two times. With DTEK power show when it goes off;
    // without DTEK power show when it comes back. Without the schedule show nothing.
    const bool dtek_on = v->kind == POWER_DTEK;
    const time_t off = h->next_off, back = h->next_on;
    char hm[8];
    v->label[0] = v->value[0] = '\0';
    if (dtek_on && off) {
        with_day(v->label, sizeof(v->label), tr(T_POWER_OFF), off, now);
        off_window(v->value, sizeof(v->value), off, back);
    } else if (dtek_on && back) {
        // The schedule has an outage now, but the power is on: it can go off at any time until then.
        strlcpy(v->label, tr(T_POWER_OFF_SCHEDULE), sizeof(v->label));
        until(v->value, sizeof(v->value), back, now);
    } else if (!dtek_on && back) {
        with_day(v->label, sizeof(v->label), tr(T_POWER_ON), back, now);
        hhmm(hm, sizeof(hm), back);
        snprintf(v->value, sizeof(v->value), tr(T_AT_TIME), hm);
    } else if (!dtek_on && off) {
        // The schedule has power now, but there is no power: an outage out of the schedule.
        strlcpy(v->label, tr(T_POWER_ON_SCHEDULE), sizeof(v->label));
        until(v->value, sizeof(v->value), off, now);
    } else if (h->schedule_known) {
        strlcpy(v->label, tr(T_POWER_SCHEDULE), sizeof(v->label));
        strlcpy(v->value, tr(T_POWER_NO_OUTAGES), sizeof(v->value));
    }
}

/* Shapes */

static void card(int x, int y, int w, int h)
{
    gfx_fill_round_rect(x, y, w, h, 10, GFX_WHITE);
    gfx_round_rect(x, y, w, h, 10, 2, GFX_BLACK);
}

// The lightning bolt of trmnl-server (Bolt).
static void bolt(float x, float y, float w, float h, gfx_color_t color)
{
    const float p[] = {
        x + w * 0.6f, y,
        x, y + h * 0.58f,
        x + w * 0.45f, y + h * 0.58f,
        x + w * 0.35f, y + h,
        x + w, y + h * 0.4f,
        x + w * 0.55f, y + h * 0.4f,
    };
    gfx_fill_polygon(p, 6, color);
}

// Battery 28 x 14 (Battery of trmnl-server).
static void battery(int x, int y, int percent, gfx_color_t color, gfx_color_t background)
{
    const int w = 28, h = 14;
    gfx_fill_rect(x, y, w, h, background);
    gfx_rect(x, y, w, h, 2, color);
    gfx_fill_rect(x + w, y + 4, 3, h - 8, color);
    if (percent > 0) {
        const int level = (percent > 100 ? 100 : percent) * (w - 6) / 100;
        gfx_fill_rect(x + 3, y + 3, level, h - 6, color);
    }
}

// Wi-Fi icon, 19 x 15 pixels, crossed: the device has no Wi-Fi connection. (cx, by) is the bottom center.
static void wifi_off_icon(int cx, int by, gfx_color_t fg, gfx_color_t bg)
{
    const float oy = (float)by - 1.5f;
    for (int y = by - 15; y <= by; y++) {
        for (int x = cx - 9; x <= cx + 9; x++) {
            const float dx = (float)(x - cx);
            const float dy = oy - (float)y;
            const float r = sqrtf(dx * dx + dy * dy);
            bool on = r <= 2.0f;
            if (!on && dy > 1.0f && fabsf(dx) <= dy) {
                for (int k = 1; k <= 3; k++) {
                    if (fabsf(r - (1.5f + 4.0f * (float)k)) <= 1.0f) {
                        on = true;
                    }
                }
            }
            if (on) {
                gfx_pixel(x, y, fg);
            }
        }
    }
    for (int t = -2; t <= 2; t++) {
        gfx_line(cx - 9 + t, by - 15, cx + 9 + t, by, bg);
    }
    gfx_line(cx - 9, by - 15, cx + 9, by, fg);
    gfx_line(cx - 8, by - 15, cx + 10, by, fg);
}

// A triangle with "!" (WarningSign of trmnl-server).
static void warning_sign(int x, int y, int size, gfx_color_t color, gfx_color_t background)
{
    const float h = size * 0.9f;
    const float p[] = { x + size / 2.0f, (float)y, (float)(x + size), y + h, (float)x, y + h };
    gfx_fill_polygon(p, 3, color);
    gfx_text(&roboto_b14, x + size / 2, y + (int)h - 3, GFX_ALIGN_CENTER, background, "!");
}

static int trend_width(const trend_t *t, const gfx_font_t *f)
{
    const int w = gfx_text_width(f, t->text);
    return t->dir == 0 ? w : w + (int)(f->size * 0.5f) + 6;
}

// Draw a trend text with a triangle before it. The text ends at right. Return the left edge (TrendText).
static int trend_text(const trend_t *t, const gfx_font_t *f, int right, int baseline, gfx_color_t color)
{
    const int w = gfx_text(f, right, baseline, GFX_ALIGN_RIGHT, color, t->text);
    if (t->dir == 0) {
        return right - w;
    }
    const float size = f->size * 0.5f, half = size / 2;
    const float cx = (float)(right - w) - size * 0.5f - 6;
    const float cy = (float)baseline - f->size * 0.36f;
    float p[6];
    if (t->dir > 0) {
        const float q[] = { cx - half, cy + half * 0.9f, cx + half, cy + half * 0.9f, cx, cy - half * 0.9f };
        memcpy(p, q, sizeof(p));
    } else {
        const float q[] = { cx - half, cy - half * 0.9f, cx + half, cy - half * 0.9f, cx, cy + half * 0.9f };
        memcpy(p, q, sizeof(p));
    }
    gfx_fill_polygon(p, 3, color);
    return (int)(cx - half);
}

// The largest bold font, 24 px or less, for the text in the width (FitBold).
static const gfx_font_t *fit_bold(const char *text, int max_width)
{
    static const gfx_font_t *const sizes[] = { &roboto_b24, &roboto_b20, &roboto_b18, &roboto_b15, &roboto_b14 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (gfx_text_width(sizes[i], text) <= max_width) {
            return sizes[i];
        }
    }
    return &roboto_b14;
}

/* Common parts */

static const ha_data_t *fresh_ha(const ui_state_t *s)
{
    const ha_data_t *h = s->ha;
    const int max_age = s->ha_max_age_s > 0 ? s->ha_max_age_s : HA_FRESH_S;
    if (!h || !h->updated || !s->now || s->now - h->updated > max_age) {
        return NULL;
    }
    return h;
}

// The data for the power state. hadata checks the age of each DTEK source itself (the check and the ping
// do not need Home Assistant), so the DTEK state stays also when the Home Assistant data is old.
static void power_input(const ui_state_t *s, ha_data_t *out)
{
    const ha_data_t *fresh = fresh_ha(s);
    if (fresh) {
        *out = *fresh;
        return;
    }
    memset(out, 0, sizeof(*out));
    out->flat_powered = -1;
    out->dtek = s->ha ? s->ha->dtek : -1;
}

void ui_power_text(const ui_state_t *s, char *buf, size_t len)
{
    ha_data_t d;
    power_view_t v = { 0 };
    power_input(s, &d);
    power_view(&d, s->now, &v);
    snprintf(buf, len, "%s", v.status);
}

static void tomorrow_text(char *buf, size_t len, const ha_data_t *h)
{
    buf[0] = '\0';
    if (!h || !h->forecast_valid) {
        return;
    }
    char temp[16] = "";
    if (h->forecast_has_temp) {
        const int t = h->forecast_temp;
        snprintf(temp, sizeof(temp), t > 0 ? " +%d°" : (t < 0 ? " −%d°" : " 0°"), abs(t));
    }
    if (h->forecast_text[0]) {
        snprintf(buf, len, tr(T_FORECAST_TOMORROW), temp, h->forecast_text);
    } else {
        snprintf(buf, len, tr(T_FORECAST_TOMORROW_TEMP), temp);
    }
}

// Date and weekday on the left of the black header. Return the header box.
static void header_left(const struct tm *t)
{
    gfx_fill_round_rect(MARGIN, MARGIN, GFX_WIDTH - 2 * MARGIN, 94, 10, GFX_BLACK);
    const int left = MARGIN + 10, y = MARGIN;
    char buf[48];
    if (t) {
        snprintf(buf, sizeof(buf), tr(T_DATE_DAY_MONTH), t->tm_mday, MONTH(t));
        gfx_text(&roboto_b30, left, y + 40, GFX_ALIGN_LEFT, GFX_WHITE, buf);
        gfx_text(&roboto_r17, left + 1, y + 66, GFX_ALIGN_LEFT, GFX_WHITE, WEEKDAY(t));
    } else {
        gfx_text(&roboto_b30, left, y + 40, GFX_ALIGN_LEFT, GFX_WHITE, tr(T_TIME_NOT_SET));
        gfx_text(&roboto_r17, left + 1, y + 66, GFX_ALIGN_LEFT, GFX_WHITE, tr(T_TIME_WAITING));
    }
}

// Footer: the year progress on the left; the Wi-Fi state, the time and the battery on the right
// (DrawCompactFooter).
static void draw_footer(const ui_state_t *s, const struct tm *t, time_t time_shown)
{
    const int baseline = GFX_HEIGHT - 12;
    const gfx_font_t *font = &roboto_r14;
    const int left = MARGIN + 4;
    int right = GFX_WIDTH - MARGIN - 4;
    char buf[96];

    if (s->usb_power) {
        // On USB power the percent is an estimate of the charge, or none.
        if (s->battery_percent >= 0) {
            snprintf(buf, sizeof(buf), tr(T_BATTERY_USB_PERCENT), (s->battery_percent + 2) / 5 * 5);
        } else {
            strlcpy(buf, "USB", sizeof(buf));
        }
        const int w = gfx_text(font, right, baseline, GFX_ALIGN_RIGHT, GFX_BLACK, buf);
        bolt((float)(right - w - 13), (float)baseline - 13, 9, 16, GFX_BLACK);
        right -= w + 23;
    } else if (s->battery_percent >= 0) {
        // Round to 5 %: a small change of the voltage must not change the screen.
        const int percent = (s->battery_percent + 2) / 5 * 5;
        if (percent <= LOW_BATTERY_PERCENT && !s->usb_power) {
            snprintf(buf, sizeof(buf), tr(T_BATTERY_CHARGE_PERCENT), percent);
            const int w = gfx_text_width(&roboto_b14, buf);
            const int px = right - w - 61, py = baseline - 16;
            gfx_fill_round_rect(px, py, w + 61, 22, 11, GFX_BLACK);
            battery(px + 10, baseline - 12, percent, GFX_WHITE, GFX_BLACK);
            gfx_text(&roboto_b14, px + w + 61 - 10, baseline, GFX_ALIGN_RIGHT, GFX_WHITE, buf);
            right = px - 8;
        } else {
            snprintf(buf, sizeof(buf), "%d %%", percent);
            const int w = gfx_text(font, right, baseline, GFX_ALIGN_RIGHT, GFX_BLACK, buf);
            battery(right - w - 36, baseline - 12, percent, GFX_BLACK, GFX_WHITE);
            right -= w + 46;
        }
    }

    if (time_shown) {
        hhmm(buf, sizeof(buf), time_shown);
        const int w = gfx_text(font, right, baseline, GFX_ALIGN_RIGHT, GFX_BLACK, buf);
        right -= w + 12;
    }
    if (s->screen == UI_SCREEN_CLOCK || s->eco) {
        // The clock screen (no Wi-Fi at all) or eco mode (Wi-Fi only at the updates): Wi-Fi is off on purpose,
        // so no Wi-Fi icon. "ЕКО" only in the first minute after the switch.
        const char *label = s->screen == UI_SCREEN_CLOCK ? tr(T_MODE_AUTONOMOUS) : (s->eco_label ? tr(T_MODE_ECO) : NULL);
        if (label) {
            const int w = gfx_text_width(&roboto_b14, label);
            gfx_fill_round_rect(right - w - 10, baseline - 15, w + 10, 19, 4, GFX_BLACK);
            gfx_text(&roboto_b14, right - 5, baseline, GFX_ALIGN_RIGHT, GFX_WHITE, label);
            right -= w + 22;
        }
    } else if (!s->wifi_connected) {
        wifi_off_icon(right - 9, baseline + 1, GFX_BLACK, GFX_WHITE);
        right -= 30;
    }

    if (!t) {
        return;
    }
    // The year progress. Use a shorter text if the full text does not fit.
    const int year = t->tm_year + 1900;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int days = leap ? 366 : 365;
    const int passed = t->tm_yday + 1;
    const int remaining = days - passed;
    const double progress = (double)passed / days;

    const int bx = left + gfx_text_width(font, tr(T_YEAR_TITLE)) + 6, by = baseline - 10, bw = 50, bh = 10;
    if (right < bx + bw + 6) {
        return;     // no room for the year progress (for example, "Потрібно зарядити" and "ЕКО")
    }
    gfx_text(font, left, baseline, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_YEAR_TITLE));
    gfx_fill_rect(bx, by, bw, bh, GFX_WHITE);
    gfx_fill_rect(bx, by, (int)(bw * progress), bh, GFX_BLACK);
    gfx_rect(bx, by, bw, bh, 1, GFX_BLACK);

    const int text_left = bx + bw + 6;
    char percent[16];
    snprintf(percent, sizeof(percent), "%d %%", (int)floor(progress * 100));
    char days_text[48];
    snprintf(days_text, sizeof(days_text), trn(T_YEAR_DAYS_LEFT, remaining), remaining);
    snprintf(buf, sizeof(buf), "%s · %s", percent, days_text);
    if (gfx_text_width(font, buf) > right - text_left) {
        snprintf(days_text, sizeof(days_text), trn(T_YEAR_DAYS, remaining), remaining);
        snprintf(buf, sizeof(buf), "%s · %s", percent, days_text);
        if (gfx_text_width(font, buf) > right - text_left) {
            strcpy(buf, percent);
        }
    }
    // With "Потрібно зарядити" and "ЕКО" even the percent can have no room: then show only the bar.
    if (gfx_text_width(font, buf) <= right - text_left) {
        gfx_text(font, text_left, baseline, GFX_ALIGN_LEFT, GFX_BLACK, buf);
    }
}

/* Dashboard */

static void draw_dashboard_header(const ha_data_t *h)
{
    const int y = MARGIN, right = GFX_WIDTH - MARGIN - 10;
    char buf[96];

    const bool has_temp = h && h->temperature.valid;
    if (has_temp) {
        format_temperature(buf, sizeof(buf), h->temperature.value);
    } else {
        strcpy(buf, "—");
    }
    gfx_text(&roboto_b40, right, y + 44, GFX_ALIGN_RIGHT, GFX_WHITE, buf);

    if (has_temp && h->temperature.has_change) {
        trend_t trend;
        make_trend(&trend, round(h->temperature.change * 10) / 10, 1, "°");
        const int trend_left = trend_text(&trend, &roboto_r14, right, y + 66, GFX_WHITE);
        gfx_text(&roboto_r14, trend_left - 6, y + 66, GFX_ALIGN_RIGHT, GFX_WHITE, tr(T_OUTDOOR_DOT));
    } else {
        gfx_text(&roboto_r14, right, y + 66, GFX_ALIGN_RIGHT, GFX_WHITE, tr(has_temp ? T_OUTDOOR : T_OUTDOOR_NA));
    }

    tomorrow_text(buf, sizeof(buf), h);
    gfx_text(&roboto_r14, right, y + 86, GFX_ALIGN_RIGHT, GFX_WHITE, buf);
}

// The alert strip (DrawAlertStrip with the compact sizes: 32 px, text 17, icon 20).
static void draw_alert_strip(const ha_data_t *h)
{
    const int x = MARGIN, y = 106, w = GFX_WIDTH - 2 * MARGIN, bh = 32;
    const int text_size = 17, icon = 20;
    const int baseline = y + bh / 2 + (int)(text_size * 0.36f + 0.5f);
    const int cx = x + w / 2;

    const bool known = h && h->alert_known;
    const bool active = known && (h->drone || h->missile);
    if (!active) {
        gfx_fill_round_rect(x, y, w, bh, 8, GFX_WHITE);
        gfx_round_rect(x, y, w, bh, 8, 2, GFX_BLACK);
        gfx_text(&roboto_r17, cx, baseline, GFX_ALIGN_CENTER, GFX_BLACK,
                 tr(known ? T_ALERT_NONE : T_ALERT_UNKNOWN));
        return;
    }

    char text[96];
    const char *kind = tr(h->drone && h->missile ? T_ALERT_BOTH : (h->missile ? T_ALERT_MISSILE : T_ALERT_DRONE));
    if (h->alert_since) {
        char hm[8];
        hhmm(hm, sizeof(hm), h->alert_since);
        snprintf(text, sizeof(text), tr(T_ALERT_SINCE), kind, hm);
    } else {
        strlcpy(text, kind, sizeof(text));
    }
    gfx_fill_round_rect(x, y, w, bh, 8, GFX_BLACK);
    // Put a warning sign before the text and center them together.
    const int text_w = gfx_text_width(&roboto_b18, text);
    const int gap = (int)(icon * 0.4f);
    const int start = cx - (icon + gap + text_w) / 2;
    warning_sign(start, y + (bh - (int)(icon * 0.9f)) / 2, icon, GFX_WHITE, GFX_BLACK);
    gfx_text(&roboto_b18, start + icon + gap, baseline, GFX_ALIGN_LEFT, GFX_WHITE, text);
}

static void draw_air_card(const ha_data_t *h, int x, int y, int w, int bh)
{
    const int left = x + 10, right = x + w - 10;
    const gfx_font_t *small = &roboto_r14, *value = &roboto_b26;
    char buf[32];

    card(x, y, w, bh);
    gfx_text(&roboto_b15, left, y + 20, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_AIR_TITLE));

    gfx_text(small, left, y + 44, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_HUMIDITY));
    if (h && h->humidity.valid && h->humidity.has_change) {
        trend_t trend;
        make_trend(&trend, round(h->humidity.change), 0, "");
        trim_day(&trend);
        trend_text(&trend, small, left + trend_width(&trend, small), y + 61, GFX_BLACK);
    }
    if (h && h->humidity.valid) {
        snprintf(buf, sizeof(buf), "%.0f %%", round(h->humidity.value));
    } else {
        strlcpy(buf, tr(T_NA), sizeof(buf));
    }
    gfx_text(value, right, y + 58, GFX_ALIGN_RIGHT, GFX_BLACK, buf);

    gfx_text(small, left, y + 86, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_AIR_AQI));
    const bool has_aqi = h && h->aqi.valid;
    const double aqi = has_aqi ? round(h->aqi.value) : 0;
    const char *note = has_aqi ? aqi_level(aqi) : "";
    gfx_text(small, left, y + 103, GFX_ALIGN_LEFT, GFX_BLACK, note);
    if (has_aqi) {
        snprintf(buf, sizeof(buf), "%.0f", aqi);
    } else {
        strlcpy(buf, tr(T_NA), sizeof(buf));
    }
    const int value_w = gfx_text(value, right, y + 100, GFX_ALIGN_RIGHT, GFX_BLACK, buf);

    // The change after the level: the full text if it fits, else only the number.
    if (has_aqi && h->aqi.has_change) {
        trend_t full, shorter;
        aqi_trend(&full, round(h->aqi.change));
        shorter = full;
        strlcpy(shorter.text, full.bare, sizeof(shorter.text));
        const int note_w = gfx_text_width(small, note);
        const int start = left + note_w + (note_w > 0 ? 8 : 0);
        const int limit = right - value_w - 6;
        const trend_t *fit = NULL;
        if (start + trend_width(&full, small) <= limit) {
            fit = &full;
        } else if (start + trend_width(&shorter, small) <= limit) {
            fit = &shorter;
        }
        if (fit) {
            trend_text(fit, small, start + trend_width(fit, small), y + 103, GFX_BLACK);
        }
    }
}

static void draw_power_card(const ui_state_t *s, const ha_data_t *h, int x, int y, int w, int bh)
{
    const int left = x + 10, right = x + w - 10;
    char buf[32];

    card(x, y, w, bh);
    bolt((float)left, (float)y + 7, 10, 16, GFX_BLACK);
    gfx_text(&roboto_b15, left + 14, y + 20, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_POWER_TITLE));
    if (s->power_queue && s->power_queue[0]) {
        gfx_text(&roboto_r13, right, y + 20, GFX_ALIGN_RIGHT, GFX_BLACK, s->power_queue);
    }

    ha_data_t d;
    power_view_t v = { 0 };
    power_input(s, &d);
    power_view(&d, s->now, &v);

    int volt_w = 0;
    if (h && h->voltage_valid) {
        snprintf(buf, sizeof(buf), tr(T_POWER_VOLTS), round(h->voltage));
        volt_w = gfx_text(&roboto_r14, right, y + 41, GFX_ALIGN_RIGHT, GFX_BLACK, buf) + 6;
    }
    // The title says "Світло": show only the source, as large as it fits (18 px at most).
    const gfx_font_t *font = &roboto_b18;
    if (gfx_text_width(font, v.source) > right - left - volt_w) {
        font = fit_bold(v.source, right - left - volt_w);
    }
    gfx_text(font, left, y + 41, GFX_ALIGN_LEFT, GFX_BLACK, v.source);

    gfx_text(&roboto_r13, left, y + 78, GFX_ALIGN_LEFT, GFX_BLACK, v.label);
    gfx_text(fit_bold(v.value, right - left), left, y + 105, GFX_ALIGN_LEFT, GFX_BLACK, v.value);
}

static void draw_dashboard(const ui_state_t *s, const struct tm *t)
{
    const ha_data_t *h = fresh_ha(s);
    header_left(t);
    draw_dashboard_header(h);
    draw_alert_strip(h);
    const int card_w = (GFX_WIDTH - 3 * MARGIN) / 2;
    draw_air_card(h, MARGIN, 144, card_w, 118);
    draw_power_card(s, h, MARGIN + card_w + MARGIN, 144, card_w, 118);
    draw_footer(s, t, h ? h->updated : s->now);
}

/* Room */

// The header with a clock: date and weekday, the time, the outdoor temperature and tomorrow.
static void clock_header(const ui_state_t *s, const struct tm *t)
{
    const ha_data_t *h = fresh_ha(s);
    const int y = MARGIN, right = GFX_WIDTH - MARGIN - 10;
    char buf[96], value[24];

    header_left(t);
    if (t) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    } else {
        strcpy(buf, "--:--");
    }
    gfx_text(&roboto_b40, right, y + 44, GFX_ALIGN_RIGHT, GFX_WHITE, buf);
    if (h && h->temperature.valid) {
        format_temperature(value, sizeof(value), h->temperature.value);
        snprintf(buf, sizeof(buf), tr(T_OUTDOOR_VALUE), value);
        gfx_text(&roboto_r14, right, y + 66, GFX_ALIGN_RIGHT, GFX_WHITE, buf);
    }
    tomorrow_text(buf, sizeof(buf), h);
    gfx_text(&roboto_r14, right, y + 86, GFX_ALIGN_RIGHT, GFX_WHITE, buf);
}

static void draw_room(const ui_state_t *s, const struct tm *t)
{
    const ha_data_t *h = fresh_ha(s);
    char buf[96];

    clock_header(s, t);

    const int cx = MARGIN, cy = 106, cw = GFX_WIDTH - 2 * MARGIN, ch = 156;
    const int left = cx + 10, middle = cx + cw / 2 + 10;
    card(cx, cy, cw, ch);
    gfx_text(&roboto_b18, left, cy + 24, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_ROOM_TITLE));
    gfx_text(&roboto_r14, left, cy + 52, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_ROOM_TEMPERATURE));
    gfx_text(&roboto_r14, middle, cy + 52, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_HUMIDITY));
    if (s->room_valid) {
        format_temperature(buf, sizeof(buf), s->room_temperature);
        gfx_text(&roboto_b40, left, cy + 100, GFX_ALIGN_LEFT, GFX_BLACK, buf);
        snprintf(buf, sizeof(buf), "%d %%", (int)(s->room_humidity + 0.5f));
        gfx_text(&roboto_b40, middle, cy + 100, GFX_ALIGN_LEFT, GFX_BLACK, buf);
    } else {
        gfx_text(&roboto_b40, left, cy + 100, GFX_ALIGN_LEFT, GFX_BLACK, "—");
        gfx_text(&roboto_b40, middle, cy + 100, GFX_ALIGN_LEFT, GFX_BLACK, "—");
    }

    // Outdoor air under the room values.
    if (h && (h->humidity.valid || h->aqi.valid)) {
        char hum[32] = "", aqi[48] = "", both[88];
        if (h->humidity.valid) {
            snprintf(hum, sizeof(hum), tr(T_OUTDOOR_HUMIDITY), round(h->humidity.value));
        }
        if (h->aqi.valid) {
            snprintf(aqi, sizeof(aqi), tr(T_AQI_VALUE_LEVEL), round(h->aqi.value), aqi_level(round(h->aqi.value)));
        }
        snprintf(both, sizeof(both), "%s%s%s", hum, hum[0] && aqi[0] ? " · " : "", aqi);
        snprintf(buf, sizeof(buf), tr(T_OUTDOOR_ROOM), both);
        gfx_text(&roboto_r14, left, cy + 138, GFX_ALIGN_LEFT, GFX_BLACK, buf);
    }

    draw_footer(s, t, 0);
}

/* Four quadrants */

// The largest bold font, max_size or less, for the text in the width.
static const gfx_font_t *fit_font(const char *text, int max_width, int max_size)
{
    static const gfx_font_t *const sizes[] = {
        &roboto_b30, &roboto_b26, &roboto_b24, &roboto_b20, &roboto_b18, &roboto_b15, &roboto_b14,
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (sizes[i]->size <= max_size && gfx_text_width(sizes[i], text) <= max_width) {
            return sizes[i];
        }
    }
    return &roboto_b14;
}

// Draw a text at the center of a quadrant column, with the largest font that fits.
static void quad_text(int cx, int baseline, int max_size, gfx_color_t color, const char *text)
{
    gfx_text(fit_font(text, 184, max_size), cx, baseline, GFX_ALIGN_CENTER, color, text);
}

static bool battery_low(const ui_state_t *s)
{
    return s->battery_percent >= 0 && (s->battery_percent + 2) / 5 * 5 <= LOW_BATTERY_PERCENT && !s->usb_power;
}

// Small icons at the top left corner: the battery (with a bolt on USB power) and "Потрібно зарядити"
// when the battery is low; at the right end "ЕКО" in eco mode or a crossed Wi-Fi icon without
// a connection. White on the black quadrant.
static void quad_icons(const ui_state_t *s)
{
    int x = 6, right = GFX_WIDTH / 2 - 6;
    const int y = 5;
    if (s->eco) {
        // Wi-Fi is off on purpose: no Wi-Fi icon. "ЕКО" only in the first minute after the switch.
        if (s->eco_label) {
            right -= gfx_text(&roboto_b14, right, y + 11, GFX_ALIGN_RIGHT, GFX_WHITE, tr(T_MODE_ECO)) + 6;
        }
    } else if (!s->wifi_connected) {
        wifi_off_icon(right - 9, y + 13, GFX_WHITE, GFX_BLACK);
        right -= 25;
    }
    // The battery is at the top right, on the white quadrant (quad_battery()).
    if (battery_low(s)) {
        const char *text = gfx_text_width(&roboto_b14, tr(T_BATTERY_CHARGE)) <= right - x ? tr(T_BATTERY_CHARGE)
                                                                                     : tr(T_BATTERY_CHARGE_SHORT);
        gfx_text(&roboto_b14, x, y + 11, GFX_ALIGN_LEFT, GFX_WHITE, text);
    }
}

// The top corners of the white quadrant, in black over the outdoor temperature: the battery at the left corner
// (on USB power with a bolt), its percent at the right corner (on USB power an estimate of the charge, or none).
// Return the left edge of the percent (GFX_WIDTH - MARGIN without a percent).
static int quad_battery(const ui_state_t *s)
{
    const int y = 3, w = 20, h = 10;
    int right = GFX_WIDTH - MARGIN;
    if (s->battery_percent < 0 && !s->usb_power) {
        return right;
    }
    const int x = GFX_WIDTH / 2 + MARGIN;
    gfx_rect(x, y, w, h, 1, GFX_BLACK);
    gfx_fill_rect(x + w, y + 3, 2, h - 6, GFX_BLACK);
    const int percent = s->battery_percent > 100 ? 100 : s->battery_percent;
    const int level = percent > 0 ? percent * (w - 4) / 100 : 0;
    if (level > 0) {
        gfx_fill_rect(x + 2, y + 2, level, h - 4, GFX_BLACK);
    }
    if (s->usb_power) {
        bolt((float)(x + w + 6), (float)y - 1, 7, 12, GFX_BLACK);
    }
    if (s->battery_percent >= 0) {
        char text[16];
        // Round to 5 %, as the footer: a small change of the voltage must not change the screen.
        snprintf(text, sizeof(text), "%d%%", (s->battery_percent + 2) / 5 * 5);
        right -= gfx_text(&roboto_b14, right, y + 10, GFX_ALIGN_RIGHT, GFX_BLACK, text);
    }
    return right;
}

// Top left, black: the date and the time ("13.11" and "9:24"), and the small icons.
static void quad_time(const ui_state_t *s, const struct tm *t)
{
    const int x = 0, y = 0, w = GFX_WIDTH / 2, h = GFX_HEIGHT / 2, cx = x + w / 2;
    char buf[16];
    gfx_fill_rect(x, y, w, h, GFX_BLACK);
    quad_icons(s);
    if (!t) {
        gfx_text(&roboto_b64d, cx, y + 78, GFX_ALIGN_CENTER, GFX_WHITE, "—");
        quad_text(cx, y + 124, 24, GFX_WHITE, tr(T_TIME_NOT_SET_SHORT));
        return;
    }
    // The digits are 46 px high: the date 22..68, the time 90..136.
    snprintf(buf, sizeof(buf), "%d.%02d", t->tm_mday, t->tm_mon + 1);
    gfx_text(&roboto_b64d, cx, y + 78, GFX_ALIGN_CENTER, GFX_WHITE, buf);
    snprintf(buf, sizeof(buf), "%d:%02d", t->tm_hour, t->tm_min);
    gfx_text(&roboto_b64d, cx, y + 146, GFX_ALIGN_CENTER, GFX_WHITE, buf);
}

// Top right, white: the outdoor temperature and humidity, and the air quality: the level in words is the main
// text ("чисте"), the AQI number is smaller under it.
static void quad_air(const ha_data_t *h)
{
    // 5 px down from the top: the top row (battery, percent, note) is over the temperature. trmnl-server
    // draws this quadrant at the same place (DrawQuadAir).
    const int x = GFX_WIDTH / 2, y = 5, cx = x + GFX_WIDTH / 4;
    char buf[32];
    if (h && h->temperature.valid) {
        format_temperature(buf, sizeof(buf), h->temperature.value);
    } else {
        strcpy(buf, "—");
    }
    gfx_text(&roboto_b56d, cx, y + 62, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    // "надворі · вологість 74 %"
    char line[64];
    if (h && h->humidity.valid) {
        char hum[32];
        snprintf(hum, sizeof(hum), tr(T_OUTDOOR_HUMIDITY), round(h->humidity.value));
        snprintf(line, sizeof(line), "%s · %s", tr(T_OUTDOOR), hum);
    } else {
        strlcpy(line, tr(T_OUTDOOR), sizeof(line));
    }
    gfx_text(&roboto_r14, cx, y + 82, GFX_ALIGN_CENTER, GFX_BLACK, line);
    if (h && h->aqi.valid) {
        const double aqi = round(h->aqi.value);
        quad_text(cx, y + 116, 30, GFX_BLACK, aqi_level(aqi));
        snprintf(buf, sizeof(buf), tr(T_AQI_VALUE), aqi);
        gfx_text(&roboto_r17, cx, y + 140, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    } else {
        quad_text(cx, y + 116, 26, GFX_BLACK, tr(T_AQI_NA));
    }
}

// Bottom left, white: the alert. An active alert has a thick frame and a warning sign.
static void quad_alert(const ha_data_t *h)
{
    const int x = 0, y = GFX_HEIGHT / 2, w = GFX_WIDTH / 2, cx = x + w / 2;
    char buf[32];
    if (!h || !h->alert_known) {
        quad_text(cx, y + 70, 24, GFX_BLACK, tr(T_QUAD_NO_ALERT_DATA_1));
        quad_text(cx, y + 104, 24, GFX_BLACK, tr(T_QUAD_NO_ALERT_DATA_2));
        return;
    }
    if (!h->drone && !h->missile) {
        quad_text(cx, y + 70, 30, GFX_BLACK, tr(T_QUAD_NO_ALERT_1));
        quad_text(cx, y + 108, 30, GFX_BLACK, tr(T_QUAD_NO_ALERT_2));
        return;
    }
    gfx_round_rect(x + 4, y + 4, w - 8, GFX_HEIGHT / 2 - 8, 8, 4, GFX_BLACK);
    // A warning sign before "Тривога", centered together.
    const int icon = 30, gap = 8;
    const int tw = gfx_text_width(&roboto_b30, tr(T_QUAD_ALERT));
    const int start = cx - (icon + gap + tw) / 2;
    warning_sign(start, y + 23, icon, GFX_BLACK, GFX_WHITE);
    gfx_text(&roboto_b30, start + icon + gap, y + 50, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_QUAD_ALERT));
    quad_text(cx, y + 86, 26, GFX_BLACK,
              tr(h->drone && h->missile ? T_QUAD_ALERT_BOTH : (h->missile ? T_QUAD_ALERT_MISSILE : T_QUAD_ALERT_DRONE)));
    if (h->alert_since) {
        char hm[8];
        hhmm(hm, sizeof(hm), h->alert_since);
        snprintf(buf, sizeof(buf), tr(T_SINCE), hm);
        gfx_text(&roboto_b30, cx, y + 128, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    }
}

// Bottom right, black: the power state and the next change.
static void quad_power(const ui_state_t *s, const ha_data_t *h)
{
    const int x = GFX_WIDTH / 2, y = GFX_HEIGHT / 2, w = GFX_WIDTH / 2, cx = x + w / 2;
    gfx_fill_rect(x, y, w, GFX_HEIGHT / 2, GFX_BLACK);
    (void)h;
    ha_data_t d;
    power_view_t v = { 0 };
    power_input(s, &d);
    power_view(&d, s->now, &v);
    quad_text(cx, y + 56, 30, GFX_WHITE, v.status);
    gfx_text(&roboto_r14, cx, y + 102, GFX_ALIGN_CENTER, GFX_WHITE, v.label);
    quad_text(cx, y + 134, 26, GFX_WHITE, v.value);
}

static void draw_quad(const ui_state_t *s, const struct tm *t)
{
    const ha_data_t *h = fresh_ha(s);
    quad_time(s, t);
    quad_air(h);
    quad_alert(h);
    quad_power(s, h);
    quad_battery(s);
}

/* Server screen */

// An image older than this gets its time at the top right.
#define SERVER_STALE_S (15 * 60)

// The screen from trmnl-server: four quadrants, the top left one empty. The device draws the date, the time
// and its icons there each minute, as on the "4 quadrants" screen. Without a fresh image (trmnl-server does not
// answer) the device draws the other quadrants itself from fresh Home Assistant data; without them it shows
// the last image with its time.
static void draw_server(const ui_state_t *s, const struct tm *t)
{
    const bool image_fresh = s->server_image &&
        (!s->now || !s->server_image_at || s->now - s->server_image_at <= SERVER_STALE_S);
    const ha_data_t *h = fresh_ha(s);
    char note[40] = "";     // at the top right, left of the battery
    if (image_fresh) {
        gfx_copy(s->server_image);
    } else if (h) {
        quad_air(h);
        quad_alert(h);
        quad_power(s, h);
        strlcpy(note, tr(T_SERVER_HA_DATA), sizeof(note));
    } else if (s->server_image) {
        char hm[8];
        gfx_copy(s->server_image);
        hhmm(hm, sizeof(hm), s->server_image_at);
        snprintf(note, sizeof(note), tr(T_SERVER_DATA_AT), hm);
    } else {
        const int cx = GFX_WIDTH * 3 / 4;
        quad_text(cx, 62, 24, GFX_BLACK, tr(T_SERVER_NO_IMAGE_1));
        quad_text(cx, 92, 24, GFX_BLACK, tr(T_SERVER_NO_IMAGE_2));
        gfx_text(&roboto_r14, cx, 122, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_SERVER_NO_IMAGE_3));
        gfx_text(&roboto_r14, cx, 140, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_SERVER_NO_IMAGE_4));
    }
    quad_time(s, t);
    const int left = quad_battery(s);
    if (note[0]) {
        gfx_text(&roboto_r13, left - 8, 13, GFX_ALIGN_RIGHT, GFX_BLACK, note);
    }
}

/* Note */

#define NOTE_MAX_LINES 12
#define NOTE_LINE_LEN  192

typedef struct {
    char text[NOTE_LINE_LEN];
} note_line_t;

static void push_line(note_line_t *lines, int max_lines, int *count, const char *text)
{
    if (*count < max_lines) {
        strlcpy(lines[*count].text, text, NOTE_LINE_LEN);
    }
    (*count)++;
}

// Break the text into lines that fit max_width: at the spaces, and at each '\n'.
// A word wider than the line stays on its own line. Return the number of lines
// (it can be larger than max_lines: then the text does not fit).
static int wrap_note(const gfx_font_t *font, const char *text, int max_width, note_line_t *lines, int max_lines)
{
    int count = 0;
    char line[NOTE_LINE_LEN] = "";
    char candidate[NOTE_LINE_LEN];
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            push_line(lines, max_lines, &count, line);
            line[0] = '\0';
            p++;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        const char *word = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            p++;
        }
        const size_t word_len = (size_t)(p - word);
        // candidate = line + " " + word, cut at the buffer size
        size_t n = strlcpy(candidate, line, sizeof(candidate));
        if (line[0] && n + 1 < sizeof(candidate)) {
            candidate[n++] = ' ';
        }
        const size_t room = n < sizeof(candidate) ? sizeof(candidate) - 1 - n : 0;
        const size_t copy = word_len < room ? word_len : room;
        memcpy(candidate + n, word, copy);
        candidate[n + copy] = '\0';
        if (!line[0] || gfx_text_width(font, candidate) <= max_width) {
            strlcpy(line, candidate, sizeof(line));
        } else {
            push_line(lines, max_lines, &count, line);
            const size_t w = word_len < sizeof(line) - 1 ? word_len : sizeof(line) - 1;
            memcpy(line, word, w);
            line[w] = '\0';
        }
    }
    if (line[0] || count == 0) {
        push_line(lines, max_lines, &count, line);
    }
    return count;
}

// The text from the settings page in a card, with the largest font that fits.
/* Clock (the autonomous mode) */

// The clock screen has no Wi-Fi and no Home Assistant data. At the top, on black: the weekday and the date,
// then the time with the largest digits. Under them: the room temperature and humidity (SHTC3).
// The footer shows the battery and "АВТОНОМНО".
static void draw_clock(const ui_state_t *s, const struct tm *t)
{
    // The 64 px digits are 46 px high, 10 px over the baseline: the time is at 84..130.
    const int band = 156, cx = GFX_WIDTH / 2, left = GFX_WIDTH / 4, right = GFX_WIDTH * 3 / 4;
    char buf[64];

    gfx_fill_rect(0, 0, GFX_WIDTH, band, GFX_BLACK);
    if (t) {
        snprintf(buf, sizeof(buf), tr(T_DATE_WEEKDAY_DAY_MONTH), WEEKDAY(t), t->tm_mday, MONTH(t));
        gfx_text(fit_font(buf, GFX_WIDTH - 2 * MARGIN - 20, 26), cx, 44, GFX_ALIGN_CENTER, GFX_WHITE, buf);
        snprintf(buf, sizeof(buf), "%d:%02d", t->tm_hour, t->tm_min);
        gfx_text(&roboto_b64d, cx, 140, GFX_ALIGN_CENTER, GFX_WHITE, buf);
    } else {
        gfx_text(&roboto_b26, cx, 44, GFX_ALIGN_CENTER, GFX_WHITE, tr(T_TIME_NOT_SET));
        gfx_text(&roboto_b64d, cx, 140, GFX_ALIGN_CENTER, GFX_WHITE, "—");
    }

    // The room climate in two columns.
    if (s->room_valid) {
        format_temperature(buf, sizeof(buf), s->room_temperature);
    } else {
        strcpy(buf, "—");
    }
    gfx_text(&roboto_b56d, left, 226, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    gfx_text(&roboto_r14, left, 248, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_CLOCK_IN_ROOM));
    if (s->room_valid) {
        snprintf(buf, sizeof(buf), "%d %%", (int)(s->room_humidity + 0.5f));
    } else {
        strcpy(buf, "—");
    }
    gfx_text(&roboto_b56d, right, 226, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    gfx_text(&roboto_r14, right, 248, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_CLOCK_HUMIDITY));
    gfx_vline(cx, band + 22, 76, GFX_BLACK);

    draw_footer(s, t, 0);
}

/* Days of the year (the extreme power saving) */

// Below 2 % battery, once a day: the date and the days of the year that passed and that are left, as the
// DaysInYear screen of trmnl-server, and on black at the bottom: charge the battery.
static void draw_days(const ui_state_t *s, const struct tm *t)
{
    const int cx = GFX_WIDTH / 2, band = 214;
    char buf[64];
    if (t) {
        const int year = t->tm_year + 1900;
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        const int passed = t->tm_yday + 1;
        const int left = (leap ? 366 : 365) - passed;
        snprintf(buf, sizeof(buf), tr(T_DATE_DAY_MONTH_YEAR), t->tm_mday, MONTH(t), year);
        gfx_text(fit_font(buf, GFX_WIDTH - 2 * MARGIN - 20, 30), cx, 64, GFX_ALIGN_CENTER, GFX_BLACK, buf);
        snprintf(buf, sizeof(buf), trn(T_DAYS_PASSED, passed), passed);
        gfx_text(&roboto_b26, cx, 126, GFX_ALIGN_CENTER, GFX_BLACK, buf);
        snprintf(buf, sizeof(buf), trn(T_DAYS_LEFT, left), left);
        gfx_text(&roboto_b26, cx, 174, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    } else {
        gfx_text(&roboto_b26, cx, 120, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_TIME_NOT_SET));
    }

    gfx_fill_rect(0, band, GFX_WIDTH, GFX_HEIGHT - band, GFX_BLACK);
    if (s->battery_percent >= 0) {
        snprintf(buf, sizeof(buf), tr(T_BATTERY_CHARGE_PERCENT), s->battery_percent);
    } else {
        strlcpy(buf, tr(T_BATTERY_CHARGE), sizeof(buf));
    }
    gfx_text(fit_font(buf, GFX_WIDTH - 2 * MARGIN - 20, 26), cx, band + 40, GFX_ALIGN_CENTER, GFX_WHITE, buf);
    gfx_text(&roboto_r14, cx, band + 70, GFX_ALIGN_CENTER, GFX_WHITE,
             tr(T_DAYS_NO_LINK));
}

static void draw_note(const ui_state_t *s, const struct tm *t)
{
    clock_header(s, t);
    const int x = MARGIN, y = 106, w = GFX_WIDTH - 2 * MARGIN, h = 156;
    card(x, y, w, h);

    const char *text = s->note ? s->note : "";
    if (!text[0]) {
        gfx_text(&roboto_r17, GFX_WIDTH / 2, y + h / 2, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_NOTE_EMPTY));
        gfx_text(&roboto_r14, GFX_WIDTH / 2, y + h / 2 + 22, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_NOTE_EMPTY_HINT));
        draw_footer(s, t, 0);
        return;
    }

    static const gfx_font_t *const fonts[] = {
        &roboto_b40, &roboto_b30, &roboto_b26, &roboto_b24, &roboto_b20, &roboto_r17, &roboto_r14, &roboto_r13,
    };
    static note_line_t lines[NOTE_MAX_LINES];
    const int pad = 10, area_w = w - 2 * pad, area_h = h - 2 * 6;
    const gfx_font_t *font = &roboto_r13;
    int count = 0, line_h = 16;
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        const int lh = (int)(fonts[i]->size * 1.25f + 0.5f);
        const int n = wrap_note(fonts[i], text, area_w, lines, NOTE_MAX_LINES);
        font = fonts[i];
        count = n;
        line_h = lh;
        if (n <= NOTE_MAX_LINES && n * lh <= area_h) {
            break;
        }
    }
    if (count > area_h / line_h) {
        count = area_h / line_h;  // the smallest font: show the lines that fit
    }
    if (count > NOTE_MAX_LINES) {
        count = NOTE_MAX_LINES;
    }

    // Center the lines vertically. The baseline is about 0.85 of the line below its top.
    const int top = y + 6 + (area_h - count * line_h) / 2;
    for (int i = 0; i < count; i++) {
        gfx_text(font, x + pad, top + i * line_h + (int)(line_h * 0.85f), GFX_ALIGN_LEFT, GFX_BLACK, lines[i].text);
    }
    draw_footer(s, t, 0);
}

/* Setup */

// The library calls a display function without a context, so the position is static.
static int s_qr_x, s_qr_y, s_qr_max;

static void draw_qr(esp_qrcode_handle_t qr)
{
    const int size = esp_qrcode_get_size(qr);
    int scale = s_qr_max / (size + 4);  // 2 modules of white border on each side
    if (scale < 1) {
        scale = 1;
    }
    const int full = (size + 4) * scale;
    const int x0 = s_qr_x - full;      // s_qr_x is the right edge
    const int y0 = s_qr_y - full / 2;  // s_qr_y is the vertical center
    gfx_fill_rect(x0, y0, full, full, GFX_WHITE);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qr, x, y)) {
                gfx_fill_rect(x0 + (x + 2) * scale, y0 + (y + 2) * scale, scale, scale, GFX_BLACK);
            }
        }
    }
}

// Return a short text for a Wi-Fi disconnection reason (wifi_err_reason_t).
static const char *wifi_reason_text(int reason)
{
    switch (reason) {
    case 0:
        return "";
    case 201:  // WIFI_REASON_NO_AP_FOUND
        return tr(T_WIFI_NO_AP);
    case 2:    // WIFI_REASON_AUTH_EXPIRE
    case 15:   // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case 202:  // WIFI_REASON_AUTH_FAIL
    case 204:  // WIFI_REASON_HANDSHAKE_TIMEOUT
        return tr(T_WIFI_AUTH);
    default:
        return tr(T_WIFI_ERROR);
    }
}

static void draw_setup(const ui_state_t *s, const struct tm *t)
{
    const int y0 = MARGIN, right = GFX_WIDTH - MARGIN - 10;
    char buf[96];

    header_left(t);
    if (t) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
        gfx_text(&roboto_b40, right, y0 + 44, GFX_ALIGN_RIGHT, GFX_WHITE, buf);
    }
    if (!s->wifi_connected && s->wifi_configured && s->wifi_reason) {
        snprintf(buf, sizeof(buf), "Wi-Fi: %s", wifi_reason_text(s->wifi_reason));
        gfx_text(&roboto_r14, right, y0 + 86, GFX_ALIGN_RIGHT, GFX_WHITE, buf);
    }

    const int x = MARGIN, y = 106, w = GFX_WIDTH - 2 * MARGIN, h = 156;
    const int left = x + 10;
    card(x, y, w, h);
    gfx_text(&roboto_b18, left, y + 24, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_SETUP_TITLE));
    gfx_text(&roboto_r14, left, y + 48, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_SETUP_STEP1A));
    gfx_text(&roboto_r14, left, y + 66, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_SETUP_STEP1B));
    gfx_text(&roboto_b20, left, y + 90, GFX_ALIGN_LEFT, GFX_BLACK, s->ap_ssid);
    const int pw = gfx_text(&roboto_r14, left, y + 112, GFX_ALIGN_LEFT, GFX_BLACK, tr(T_SETUP_PASSWORD));
    gfx_text(&roboto_b20, left + pw + 6, y + 112, GFX_ALIGN_LEFT, GFX_BLACK, s->ap_password);
    snprintf(buf, sizeof(buf), tr(T_SETUP_STEP2), s->ap_ip);
    gfx_text(&roboto_r14, left, y + 136, GFX_ALIGN_LEFT, GFX_BLACK, buf);

    // Wi-Fi QR code (the format that phone cameras know).
    snprintf(buf, sizeof(buf), "WIFI:T:WPA;S:%s;P:%s;;", s->ap_ssid, s->ap_password);
    s_qr_x = x + w - 8;
    s_qr_y = y + h / 2;
    s_qr_max = h - 12;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = draw_qr;
    cfg.max_qrcode_version = 5;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    esp_qrcode_generate(&cfg, buf);

    draw_footer(s, t, 0);
}

static void draw_message(const char *text)
{
    const int w = 340, h = 80;
    const int x = (GFX_WIDTH - w) / 2, y = (GFX_HEIGHT - h) / 2;
    gfx_fill_round_rect(x, y, w, h, 12, GFX_BLACK);
    gfx_text(&roboto_b20, GFX_WIDTH / 2, y + 48, GFX_ALIGN_CENTER, GFX_WHITE, text);
}

/* Weather icons and the forecast screens */

// A filled disc.
static void fill_disc(int cx, int cy, int r, gfx_color_t color)
{
    const float rr = ((float)r + 0.5f) * ((float)r + 0.5f);
    for (int dy = -r; dy <= r; dy++) {
        const int dx = (int)sqrtf(rr - (float)(dy * dy));
        gfx_hline(cx - dx, cy + dy, 2 * dx + 1, color);
    }
}

// A cloud of three circles on a flat bottom, 27 x 18 px around (cx, cy). grow makes it larger on all sides.
static void cloud_shape(int cx, int cy, int grow, gfx_color_t color)
{
    fill_disc(cx - 7, cy + 2, 5 + grow, color);
    fill_disc(cx + 1, cy - 2, 8 + grow, color);
    fill_disc(cx + 8, cy + 2, 5 + grow, color);
    gfx_fill_rect(cx - 7, cy + 2, 16, 6 + grow, color);
}

// A white cloud with a black line of 2 px. It covers what is under it (the sun or the moon).
static void cloud(int cx, int cy)
{
    cloud_shape(cx, cy, 2, GFX_BLACK);
    cloud_shape(cx, cy, 0, GFX_WHITE);
}

static void sun(int cx, int cy, int r)
{
    fill_disc(cx, cy, r, GFX_BLACK);
    for (int k = 0; k < 8; k++) {
        const float a = (float)k * 3.14159265f / 4.0f;
        const float c = cosf(a), s = sinf(a);
        const int x0 = cx + (int)lroundf(c * (float)(r + 3)), y0 = cy + (int)lroundf(s * (float)(r + 3));
        const int x1 = cx + (int)lroundf(c * (float)(r + 6)), y1 = cy + (int)lroundf(s * (float)(r + 6));
        gfx_line(x0, y0, x1, y1, GFX_BLACK);
        // A second line next to it: the rays are 2 px wide.
        if (fabsf(s) > 0.5f) {
            gfx_line(x0 + 1, y0, x1 + 1, y1, GFX_BLACK);
        } else {
            gfx_line(x0, y0 + 1, x1, y1 + 1, GFX_BLACK);
        }
    }
}

// A crescent: a black disc, and a white disc over its top right part.
static void moon(int cx, int cy, int r)
{
    fill_disc(cx, cy, r, GFX_BLACK);
    fill_disc(cx + r * 2 / 3, cy - r / 3, r * 4 / 5, GFX_WHITE);
}

static void sky_body(int cx, int cy, int r, bool night)
{
    if (night) {
        moon(cx, cy, r + 2);
    } else {
        sun(cx, cy, r);
    }
}

// The marks of the precipitation, 9 px high from y.
static void drop(int x, int y)
{
    for (int k = 0; k < 3; k++) {
        gfx_line(x + 3 + k, y, x + k, y + 8, GFX_BLACK);
    }
}

static void flake(int x, int y)
{
    gfx_fill_rect(x, y, 2, 9, GFX_BLACK);
    for (int k = 0; k < 2; k++) {
        gfx_line(x - 3, y + 2 + k, x + 4, y + 6 + k, GFX_BLACK);
        gfx_line(x - 3, y + 6 + k, x + 4, y + 2 + k, GFX_BLACK);
    }
}

static void dot(int x, int y)
{
    fill_disc(x, y + 4, 3, GFX_BLACK);
}

// The FMI SmartSymbol of a weather: 1..99 by day, the same + 100 at night.
//   1 clear, 2 mostly clear, 4 partly cloudy, 6 mostly cloudy, 7 overcast, 9 fog;
//   11 drizzle, 14 freezing drizzle, 17 freezing rain;
//   21..27 showers, 31..39 rain, 41..49 sleet, 51..59 snow, 61..67 hail, 71..77 thunder. In these groups the last
//   digit gives the cover (1..3 isolated, 4..6 scattered, 7..9 everywhere) and the intensity (1, 4, 7 light;
//   2, 5, 8 moderate; 3, 6, 9 heavy).
typedef enum {
    MARK_NONE = 0,
    MARK_DROP,
    MARK_SLEET,
    MARK_FLAKE,
    MARK_DOT,
    MARK_BOLT,
} mark_t;

// A weather icon, 34 x 34 px around (cx, cy).
static void weather_icon(int cx, int cy, int symbol)
{
    const bool night = symbol > 100;
    const int b = symbol % 100;
    if (b <= 0) {
        return;
    }
    if (b == 1) {
        sky_body(cx, cy, 8, night);
        return;
    }
    if (b == 2 || b == 4) {
        sky_body(cx - 6, cy - 6, 6, night);
        cloud(cx + 4, cy + 5);
        return;
    }
    if (b == 6) {
        sky_body(cx - 8, cy - 8, 5, night);
        cloud(cx + 2, cy + 3);
        return;
    }
    if (b == 7) {
        cloud(cx, cy + 2);
        return;
    }
    if (b == 9) {
        cloud(cx, cy - 5);
        gfx_fill_rect(cx - 13, cy + 9, 22, 2, GFX_BLACK);
        gfx_fill_rect(cx - 9, cy + 14, 22, 2, GFX_BLACK);
        return;
    }

    mark_t mark = MARK_DROP;
    int count = 2;
    bool body = false;              // the sun or the moon behind the cloud: isolated and scattered showers
    if (b < 20) {
        mark = b == 11 ? MARK_DOT : MARK_DROP;
        count = b == 11 ? 3 : 2;
    } else {
        const int unit = b % 10, group = b / 10;
        const int cover = unit >= 1 ? (unit - 1) / 3 : 2;
        const int intensity = unit >= 1 ? (unit - 1) % 3 : 0;
        body = cover < 2;
        switch (group) {
        case 2:
            mark = MARK_DROP;
            break;
        case 3:
            mark = MARK_DROP;
            count = intensity + 1;
            break;
        case 4:
            mark = MARK_SLEET;
            count = intensity + 2;
            break;
        case 5:
            mark = MARK_FLAKE;
            count = intensity + 1;
            break;
        case 6:
            mark = MARK_DOT;
            break;
        case 7:
            mark = MARK_BOLT;
            count = 1;
            break;
        default:
            mark = MARK_NONE;
            break;
        }
    }
    int ccx = cx;
    if (body) {
        sky_body(cx - 7, cy - 11, 5, night);
        ccx = cx + 3;
    }
    cloud(ccx, cy - 5);
    if (mark == MARK_BOLT) {
        bolt((float)(ccx - 4), (float)(cy + 5), 9, 12, GFX_BLACK);
        return;
    }
    static const int offsets[3][3] = { { 0 }, { -6, 6 }, { -11, 0, 11 } };
    if (count > 3) {
        count = 3;
    }
    for (int i = 0; i < count; i++) {
        const int x = ccx + offsets[count - 1][i], y = cy + 8;
        switch (mark) {
        case MARK_DROP:
            drop(x - 2, y);
            break;
        case MARK_FLAKE:
            flake(x, y);
            break;
        case MARK_DOT:
            dot(x, y);
            break;
        case MARK_SLEET:
            // Drops and flakes in turn.
            if (i % 2 == 0) {
                drop(x - 2, y);
            } else {
                flake(x, y);
            }
            break;
        default:
            break;
        }
    }
}

// The weather of an FMI SmartSymbol in words: "хмарно", "місцями невеликий сніг".
static void weather_text(char *buf, size_t len, int symbol)
{
    const int b = symbol % 100;
    buf[0] = '\0';
    switch (b) {
    case 1:
        strlcpy(buf, tra(T_WEATHER_SKY, 0), len);
        return;
    case 2:
        strlcpy(buf, tra(T_WEATHER_SKY, 1), len);
        return;
    case 4:
        strlcpy(buf, tra(T_WEATHER_SKY, 2), len);
        return;
    case 6:
        strlcpy(buf, tra(T_WEATHER_SKY, 3), len);
        return;
    case 7:
        strlcpy(buf, tra(T_WEATHER_SKY, 4), len);
        return;
    case 9:
        strlcpy(buf, tra(T_WEATHER_SKY, 5), len);
        return;
    case 11:
    case 14:
    case 17:
        strlcpy(buf, tra(T_WEATHER_DRIZZLE, (b - 11) / 3), len);
        return;
    default:
        break;
    }
    if (b < 21 || b > 79 || b % 10 == 0) {
        return;
    }
    const int unit = b % 10;
    const int cover = (unit - 1) / 3, intensity = (unit - 1) % 3;
    const char *base;
    switch (b / 10) {
    case 2:
        base = tr(T_WEATHER_SHOWERS);
        break;
    case 3:
        base = tra(T_WEATHER_RAIN, intensity);
        break;
    case 4:
        base = tra(T_WEATHER_SLEET, intensity);
        break;
    case 5:
        base = tra(T_WEATHER_SNOW, intensity);
        break;
    case 6:
        base = tr(T_WEATHER_HAIL);
        break;
    default:
        base = tr(T_WEATHER_THUNDER);
        break;
    }
    if (cover == 0) {
        snprintf(buf, len, tr(T_WEATHER_ISOLATED), base);
    } else if (cover == 1) {
        snprintf(buf, len, tr(T_WEATHER_SCATTERED), base);
    } else {
        strlcpy(buf, base, len);
    }
}

// "+1°", "0°", "−4°".
static void format_degrees(char *buf, size_t len, int t)
{
    snprintf(buf, len, "%s%d°", t > 0 ? "+" : (t < 0 ? "−" : ""), abs(t));
}

// The local date of today + i days (at noon). Return false without the time.
static bool day_tm(time_t now, int i, struct tm *out)
{
    if (!now) {
        return false;
    }
    localtime_r(&now, out);
    out->tm_mday += i;
    out->tm_hour = 12;
    out->tm_min = 0;
    out->tm_sec = 0;
    out->tm_isdst = -1;
    mktime(out);
    return true;
}

static bool day_known(const ha_data_t *h, int i)
{
    return h && (h->days[i].night.valid || h->days[i].day.valid);
}

// The icon and the temperature of the night or the day. Without a value: a dash.
static void day_point(int cx, int icon_cy, int baseline, const gfx_font_t *font, const hadata_point_t *p)
{
    char buf[16];
    if (!p->valid) {
        gfx_text(font, cx, baseline, GFX_ALIGN_CENTER, GFX_BLACK, "—");
        return;
    }
    weather_icon(cx, icon_cy, p->symbol);
    format_degrees(buf, sizeof(buf), p->temp);
    gfx_text(font, cx, baseline, GFX_ALIGN_CENTER, GFX_BLACK, buf);
}

// "оберіть місто / у налаштуваннях" or "немає прогнозу" in the middle of a box.
static void no_forecast(const ui_state_t *s, int cx, int cy)
{
    if (!s->forecast_city) {
        gfx_text(&roboto_r14, cx, cy - 2, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_DAYS_NO_CITY));
        gfx_text(&roboto_r14, cx, cy + 16, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_DAYS_NO_CITY_2));
    } else {
        gfx_text(&roboto_r14, cx, cy + 6, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_DAYS_NONE));
    }
}

// A panel of the 5-panel screen, 133 x 150 px: the name of the day and its date, the night and the day (icon and
// temperature, as on meteo.gov.ua) and the weather of the day in words.
static void forecast_panel(const ui_state_t *s, int i, int x, int w)
{
    const int y = GFX_HEIGHT / 2, cx = x + w / 2;
    const ha_data_t *h = s->ha;
    char buf[64];
    struct tm d;
    if (day_tm(s->now, i, &d)) {
        const char *title = i == 0 ? tr(T_DAYS_TODAY)
                                   : (i == 1 ? tr(T_DAYS_TOMORROW) : tra(T_DATE_WEEKDAYS_TITLE, d.tm_wday));
        gfx_text(fit_font(title, w - 8, 20), cx, y + 22, GFX_ALIGN_CENTER, GFX_BLACK, title);
        snprintf(buf, sizeof(buf), tr(T_DATE_DAY_MONTH), d.tm_mday, MONTH(&d));
        gfx_text(&roboto_r13, cx, y + 40, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    }
    if (!day_known(h, i)) {
        no_forecast(s, cx, y + 90);
        return;
    }
    const hadata_day_t *day = &h->days[i];
    day_point(cx - w / 4, y + 66, y + 110, &roboto_b24, &day->night);
    day_point(cx + w / 4, y + 66, y + 110, &roboto_b24, &day->day);
    weather_text(buf, sizeof(buf), day->day.valid ? day->day.symbol : day->night.symbol);
    note_line_t lines[2];
    const int n = wrap_note(&roboto_r13, buf, w - 8, lines, 2);
    for (int k = 0; k < n && k < 2; k++) {
        gfx_text(&roboto_r13, cx, y + 128 + k * 16, GFX_ALIGN_CENTER, GFX_BLACK, lines[k].text);
    }
}

// Five panels: the time and the outdoor air at the top, as on the "4 quadrants" screen; at the bottom the forecast
// for today, tomorrow and the day after tomorrow.
static void draw_quad5(const ui_state_t *s, const struct tm *t)
{
    quad_time(s, t);
    quad_air(fresh_ha(s));
    // A line under the white quadrant, and lines between the panels.
    gfx_fill_rect(GFX_WIDTH / 2, GFX_HEIGHT / 2, GFX_WIDTH / 2, 2, GFX_BLACK);
    for (int i = 0; i < 3; i++) {
        const int x = i * GFX_WIDTH / 3, w = (i + 1) * GFX_WIDTH / 3 - x;
        if (i > 0) {
            gfx_fill_rect(x - 1, GFX_HEIGHT / 2 + 10, 2, GFX_HEIGHT / 2 - 20, GFX_BLACK);
        }
        forecast_panel(s, i, x, w);
    }
    quad_battery(s);
}

#define CELL 100    // the 10-day screen: 4 x 3 squares of 100 px

// Top left, black: the weekday and the date, the time, and the battery and the state of Wi-Fi.
static void days_time_cell(const ui_state_t *s, const struct tm *t)
{
    char buf[32];
    gfx_fill_rect(0, 0, CELL, CELL, GFX_BLACK);
    if (t) {
        snprintf(buf, sizeof(buf), tr(T_DAYS_SHORT_DATE), tra(T_DATE_WEEKDAYS_SHORT, t->tm_wday), t->tm_mday,
                 t->tm_mon + 1);
        gfx_text(fit_font(buf, CELL - 8, 18), CELL / 2, 21, GFX_ALIGN_CENTER, GFX_WHITE, buf);
        snprintf(buf, sizeof(buf), "%d:%02d", t->tm_hour, t->tm_min);
        gfx_text(gfx_text_width(&roboto_b40, buf) <= CELL - 6 ? &roboto_b40 : &roboto_b30, CELL / 2, 64,
                 GFX_ALIGN_CENTER, GFX_WHITE, buf);
    } else {
        gfx_text(&roboto_b40, CELL / 2, 64, GFX_ALIGN_CENTER, GFX_WHITE, "—");
    }
    // The battery at the bottom left (with a bolt on USB power), its percent; at the right "ЕКО" or a crossed
    // Wi-Fi icon, as on the "4 quadrants" screen.
    const int y = 80;
    int x = 8;
    if (s->battery_percent >= 0 || s->usb_power) {
        const int w = 18, hgt = 9;
        gfx_rect(x, y, w, hgt, 1, GFX_WHITE);
        gfx_fill_rect(x + w, y + 3, 2, hgt - 6, GFX_WHITE);
        const int percent = s->battery_percent > 100 ? 100 : s->battery_percent;
        const int level = percent > 0 ? percent * (w - 4) / 100 : 0;
        if (level > 0) {
            gfx_fill_rect(x + 2, y + 2, level, hgt - 4, GFX_WHITE);
        }
        x += w + 5;
        if (s->usb_power) {
            bolt((float)x, (float)y - 2, 7, 12, GFX_WHITE);
            x += 10;
        }
        if (s->battery_percent >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", (s->battery_percent + 2) / 5 * 5);
            gfx_text(&roboto_b14, x, y + 9, GFX_ALIGN_LEFT, GFX_WHITE, buf);
        }
    }
    if (s->eco) {
        if (s->eco_label) {
            gfx_text(&roboto_b14, CELL - 6, y + 9, GFX_ALIGN_RIGHT, GFX_WHITE, tr(T_MODE_ECO));
        }
    } else if (!s->wifi_connected) {
        wifi_off_icon(CELL - 15, y + 11, GFX_WHITE, GFX_BLACK);
    }
}

// The weather now: the outdoor temperature, the humidity and the AQI.
static void days_now_cell(const ha_data_t *h, int x, int y)
{
    const int cx = x + CELL / 2;
    char buf[48];
    gfx_text(&roboto_b15, cx, y + 18, GFX_ALIGN_CENTER, GFX_BLACK, tr(T_DAYS_NOW));
    if (h && h->temperature.valid) {
        format_temperature(buf, sizeof(buf), h->temperature.value);
    } else {
        strcpy(buf, "—");
    }
    gfx_text(fit_font(buf, CELL - 6, 30), cx, y + 54, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    if (h && h->humidity.valid) {
        snprintf(buf, sizeof(buf), tr(T_OUTDOOR_HUMIDITY), round(h->humidity.value));
        gfx_text(&roboto_r13, cx, y + 74, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    }
    if (h && h->aqi.valid) {
        snprintf(buf, sizeof(buf), tr(T_AQI_VALUE), round(h->aqi.value));
        gfx_text(&roboto_r13, cx, y + 91, GFX_ALIGN_CENTER, GFX_BLACK, buf);
    }
}

// A day of the 10-day screen: "Сьогодні", "Завтра" or "Ср, 26.11"; the night and the day under it.
static void days_cell(const ui_state_t *s, int i, int x, int y)
{
    const int cx = x + CELL / 2;
    const ha_data_t *h = s->ha;
    char buf[32];
    struct tm d;
    if (day_tm(s->now, i, &d)) {
        const char *title = buf;
        if (i == 0) {
            title = tr(T_DAYS_TODAY);
        } else if (i == 1) {
            title = tr(T_DAYS_TOMORROW);
        } else {
            snprintf(buf, sizeof(buf), tr(T_DAYS_SHORT_DATE), tra(T_DATE_WEEKDAYS_SHORT, d.tm_wday), d.tm_mday,
                     d.tm_mon + 1);
        }
        gfx_text(fit_font(title, CELL - 6, 15), cx, y + 17, GFX_ALIGN_CENTER, GFX_BLACK, title);
    }
    if (!day_known(h, i)) {
        gfx_text(&roboto_b24, cx, y + 66, GFX_ALIGN_CENTER, GFX_BLACK, "—");
        return;
    }
    day_point(cx - 24, y + 45, y + 89, &roboto_b20, &h->days[i].night);
    day_point(cx + 24, y + 45, y + 89, &roboto_b20, &h->days[i].day);
}

// 12 squares: the time, the weather now and 10 days of the forecast (the night and the day, as on meteo.gov.ua).
static void draw_days10(const ui_state_t *s, const struct tm *t)
{
    days_time_cell(s, t);
    days_now_cell(fresh_ha(s), CELL, 0);
    // Without a forecast the two squares at the top right tell why; the other days show dashes.
    const bool none = !s->forecast_city || !s->ha || !s->ha->days_at;
    if (none) {
        no_forecast(s, 3 * CELL, CELL / 2);
    }
    for (int i = 0; i < HADATA_DAYS; i++) {
        const int n = i + 2;
        if (none && n < 4) {
            continue;
        }
        days_cell(s, i, (n % 4) * CELL, (n / 4) * CELL);
    }
    // The lines between the squares.
    for (int k = 1; k < 4; k++) {
        const int top = k == 1 || (k == 3 && none) ? CELL : 0;
        gfx_vline(k * CELL, top, GFX_HEIGHT - top, GFX_BLACK);
    }
    gfx_hline(CELL, CELL, GFX_WIDTH - CELL, GFX_BLACK);
    gfx_hline(0, 2 * CELL, GFX_WIDTH, GFX_BLACK);
}

void ui_render(const ui_state_t *s)
{
    struct tm local;
    const struct tm *t = NULL;
    if (s->now > 0) {
        localtime_r(&s->now, &local);
        t = &local;
    }

    gfx_clear(GFX_WHITE);
    if (s->ap_active) {
        draw_setup(s, t);
    } else if (s->screen == UI_SCREEN_ROOM) {
        draw_room(s, t);
    } else if (s->screen == UI_SCREEN_NOTE) {
        draw_note(s, t);
    } else if (s->screen == UI_SCREEN_QUAD) {
        draw_quad(s, t);
    } else if (s->screen == UI_SCREEN_QUAD5) {
        draw_quad5(s, t);
    } else if (s->screen == UI_SCREEN_DAYS10) {
        draw_days10(s, t);
    } else if (s->screen == UI_SCREEN_SERVER) {
        draw_server(s, t);
    } else if (s->screen == UI_SCREEN_CLOCK) {
        draw_clock(s, t);
    } else if (s->screen == UI_SCREEN_DAYS) {
        draw_days(s, t);
    } else {
        draw_dashboard(s, t);
    }
    if (s->message) {
        draw_message(s->message);
    }
}
