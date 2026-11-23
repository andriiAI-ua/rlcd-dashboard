// Settings page: a form for Wi-Fi, DNS, MQTT and the page password.
// "Save" writes the settings to NVS and restarts the device.
#include "web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "audio.h"
#include "esp_https_server.h"
#include "fwupdate.h"
#include "hadata.h"
#include "texts.h"
#include "lwip/sockets.h"
#include "tlscert.h"
#include "net.h"

static const char *TAG = "web";

#define MAX_BODY 8192

static settings_t *s_settings;  // the settings of main: the screen can change at run time
static const char *s_version;
static web_status_fn_t s_status;
static web_change_cb_t s_on_change;
static web_show_cb_t s_on_show;
static web_set_cb_t s_on_set;
static web_sound_cb_t s_on_sound;
static web_wait_cb_t s_wait_applied;
static volatile int64_t s_last_request_us;
static bool s_https;                    // the HTTPS server runs (a certificate is installed)
static char *s_tls_cert, *s_tls_key;    // the HTTPS server uses them while it runs

// True after a send error in the current answer (for example, the browser closed the connection).
// The HTTP server runs the handlers one at a time, so one flag is sufficient.
static bool s_send_failed;

/* Text helpers */

static esp_err_t send_str(httpd_req_t *req, const char *text)
{
    if (s_send_failed) {
        return ESP_FAIL;
    }
    // An empty chunk ends a chunked response, so do not send an empty text.
    if (!text || text[0] == '\0') {
        return ESP_OK;
    }
    const esp_err_t err = httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        s_send_failed = true;
    }
    return err;
}

static void begin_page(httpd_req_t *req)
{
    s_send_failed = false;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
}

// End the chunked answer. After a send error, return ESP_FAIL: the server then closes the socket.
static esp_err_t end_page(httpd_req_t *req)
{
    return s_send_failed ? ESP_FAIL : httpd_resp_send_chunk(req, NULL, 0);
}

// Send a text with the HTML special characters replaced.
static void send_escaped(httpd_req_t *req, const char *text)
{
    char buf[128];
    size_t n = 0;
    for (const char *p = text; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        const size_t need = rep ? strlen(rep) : 1;
        if (n + need >= sizeof(buf)) {
            buf[n] = '\0';
            send_str(req, buf);
            n = 0;
        }
        if (rep) {
            memcpy(buf + n, rep, need);
        } else {
            buf[n] = *p;
        }
        n += need;
    }
    buf[n] = '\0';
    if (n) {
        send_str(req, buf);
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Copy the note text: remove '\r' (the browser sends "\r\n"), and do not cut a UTF-8 character
// when the text is too long.
static void copy_note(char *dst, const char *src, size_t size)
{
    size_t n = 0;
    for (const char *p = src; *p && n + 1 < size; p++) {
        if (*p != '\r') {
            dst[n++] = *p;
        }
    }
    // If the end is in the middle of a character, remove the start of that character.
    size_t start = n;
    while (start > 0 && ((unsigned char)dst[start - 1] & 0xC0) == 0x80) {
        start--;
    }
    if (start > 0 && ((unsigned char)dst[start - 1] & 0x80)) {
        const unsigned char lead = (unsigned char)dst[start - 1];
        const size_t need = lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : 2);
        if (n - (start - 1) < need) {
            n = start - 1;
        }
    }
    dst[n] = '\0';
}

// Decode a form value ("a+b%20c") in place.
static void url_decode(char *s)
{
    char *out = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *out++ = ' ';
        } else if (*p == '%' && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
            *out++ = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
            p += 2;
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64_decode(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        const int v = base64_value(*in);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= cap) {
                return false;
            }
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[n] = '\0';
    return true;
}

/* Access */

// Return true when the request came through the setup access point.
static bool from_access_point(httpd_req_t *req)
{
    if (!net_ap_active()) {
        return false;
    }
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return false;
    }
    uint32_t local;
    if (addr.ss_family == AF_INET) {
        local = ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    } else if (addr.ss_family == AF_INET6) {
        // The server socket is IPv6: an IPv4 client has the address ::ffff:a.b.c.d.
        local = ((struct sockaddr_in6 *)&addr)->sin6_addr.un.u32_addr[3];
    } else {
        return false;
    }
    esp_ip4_addr_t ap;
    return esp_netif_str_to_ip4(net_ap_ip(), &ap) == ESP_OK && local == ap.addr;
}

// With a certificate, the home network uses only HTTPS. The access point stays on HTTP:
// phones open the captive portal only over HTTP. The HTTPS handlers have user_ctx 1.
static bool wants_https(httpd_req_t *req)
{
    return s_https && req->user_ctx == NULL && !from_access_point(req);
}

static bool authorized(httpd_req_t *req)
{
    s_last_request_us = esp_timer_get_time();
    if (wants_https(req)) {
        return false;   // deny() sends the browser to HTTPS
    }
    // Without a page password the page is open on the home network too.
    if (s_settings->web_pass[0] == '\0' || from_access_point(req)) {
        return true;
    }
    char header[200];
    char decoded[140];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK ||
        strncmp(header, "Basic ", 6) != 0 || !base64_decode(header + 6, decoded, sizeof(decoded))) {
        return false;
    }
    return strncmp(decoded, "admin:", 6) == 0 && strcmp(decoded + 6, s_settings->web_pass) == 0;
}

// Send the browser to the same address over HTTPS: the name (or IP address) that it used, without a port.
static esp_err_t redirect_https(httpd_req_t *req)
{
    char host[72] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        strlcpy(host, s_settings->host, sizeof(host));
    }
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }
    char location[sizeof(host) + 16];
    snprintf(location, sizeof(location), "https://%s/", host);
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_sendstr(req, "HTTPS");
}

static esp_err_t deny(httpd_req_t *req)
{
    if (wants_https(req)) {
        return redirect_https(req);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"RLCD\", charset=\"UTF-8\"");
    return httpd_resp_sendstr(req, tr(T_PAGE_NEED_AUTH));
}

/* Page */

// The head of each page, after "<html lang=...>" (see send_head()).
static const char PAGE_HEAD[] =
    "<head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>RLCD Dashboard</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:12px;color:#111;background:#fff}"
    "h1{font-size:22px}fieldset{border:1px solid #ccc;border-radius:8px;margin:0 0 12px;padding:8px 12px}"
    "legend{font-weight:600}label{display:block;margin:10px 0 3px;font-size:14px}"
    "input[type=text],input[type=password],input[type=number],input[type=url],textarea{width:100%;box-sizing:border-box;"
    "padding:8px;font-size:16px}"
    "label.c{display:inline;margin-left:4px;font-size:13px;color:#555}"
    "button{width:100%;padding:12px;font-size:16px;margin:8px 0}"
    "pre{background:#f3f3f3;padding:8px;border-radius:8px;white-space:pre-wrap;font-size:13px}"
    "small{color:#666}h2{font-size:17px;margin:12px 0 4px}label.r{margin:8px 0;font-size:15px}"
    ".scr button{width:auto;display:inline-block;margin:4px 6px 4px 0;padding:10px 16px}"
    ".scr button.on{font-weight:700;outline:2px solid currentColor}"
    ".off{position:sticky;top:0;background:#fff3cd;border:1px solid #d9a400;border-radius:8px;padding:10px;"
    "margin:0 0 12px;font-size:15px}"
    "</style>";

// The start of a page: the language, the style and L, the texts of the page scripts (js.* of the language files).
static void send_head(httpd_req_t *req)
{
    send_str(req, "<!doctype html><html lang=\"");
    send_str(req, texts_language());
    send_str(req, "\">");
    send_str(req, PAGE_HEAD);
    send_str(req, "<script>var L=");
    send_str(req, texts_js());
    send_str(req, ";</script></head><body><h1>RLCD Dashboard</h1>");
}

// Send a text of the language files as the text of an HTML attribute.
static void send_attr(httpd_req_t *req, text_id_t id)
{
    send_escaped(req, tr(id));
}

static void text_field(httpd_req_t *req, const char *name, text_id_t label, const char *value,
                       int max, const char *placeholder)
{
    char buf[64];
    send_str(req, "<label>");
    send_str(req, tr(label));
    send_str(req, "</label><input type=\"text\" name=\"");
    send_str(req, name);
    snprintf(buf, sizeof(buf), "\" maxlength=\"%d\" placeholder=\"", max);
    send_str(req, buf);
    send_escaped(req, placeholder);
    send_str(req, "\" value=\"");
    send_escaped(req, value);
    send_str(req, "\">");
}

// A password is never sent to the browser. An empty field keeps the old password.
static void password_field(httpd_req_t *req, const char *name, text_id_t label, const char *value, int max)
{
    char buf[64];
    send_str(req, "<label>");
    send_str(req, tr(label));
    send_str(req, "</label><input type=\"password\" name=\"");
    send_str(req, name);
    snprintf(buf, sizeof(buf), "\" maxlength=\"%d\" placeholder=\"", max);
    send_str(req, buf);
    send_attr(req, value[0] ? T_PAGE_PASSWORD_SET : T_PAGE_PASSWORD_UNSET);
    send_str(req, "\"><input type=\"checkbox\" name=\"");
    send_str(req, name);
    send_str(req, "_clear\" id=\"");
    send_str(req, name);
    send_str(req, "_clear\"><label class=\"c\" for=\"");
    send_str(req, name);
    send_str(req, "_clear\">");
    send_str(req, tr(T_PAGE_NO_PASSWORD));
    send_str(req, "</label>");
}

typedef struct {
    const char *id;
    const char *warning;    // the script text of a question before the switch ("L.warn_clock"), or NULL
} screen_t;

// In the order of ui_screen_t: the names are the list screen.names, the notes page.screen_about.
static const screen_t SCREENS[] = {
    { "dashboard", NULL }, { "room", NULL }, { "note", NULL }, { "quad", NULL },
    { "quad5", "L.warn_quad5" }, { "days10", NULL }, { "server", "L.warn_server" }, { "clock", "L.warn_clock" },
};
_Static_assert(sizeof(SCREENS) / sizeof(SCREENS[0]) == T_SCREEN_NAMES_ITEMS, "lang: screen.names");
_Static_assert(sizeof(SCREENS) / sizeof(SCREENS[0]) == T_PAGE_SCREEN_ABOUT_ITEMS, "lang: page.screen_about");

// One button for each screen. The button of the current screen is marked.
static void screen_buttons(httpd_req_t *req, const char *current)
{
    for (size_t i = 0; i < sizeof(SCREENS) / sizeof(SCREENS[0]); i++) {
        if (!settings_screen_available(s_settings, SCREENS[i].id)) {
            continue;
        }
        send_str(req, strcmp(current, SCREENS[i].id) == 0 ? "<button class=\"on\" name=\"screen\" value=\""
                                                           : "<button name=\"screen\" value=\"");
        send_str(req, SCREENS[i].id);
        send_str(req, "\" title=\"");
        send_escaped(req, tra(T_PAGE_SCREEN_ABOUT, (int)i));
        // The clock and the server screen turn Wi-Fi off: then the page does not open. Ask before.
        if (SCREENS[i].warning) {
            send_str(req, "\" onclick=\"return confirm(");
            send_str(req, SCREENS[i].warning);
            send_str(req, ")\">");
        } else {
            send_str(req, "\">");
        }
        send_str(req, tra(T_SCREEN_NAMES, (int)i));
        send_str(req, "</button>");
    }
}

static bool valid_screen(const char *id)
{
    for (size_t i = 0; i < sizeof(SCREENS) / sizeof(SCREENS[0]); i++) {
        if (strcmp(id, SCREENS[i].id) == 0) {
            return settings_screen_available(s_settings, id);
        }
    }
    return false;
}

// A yes/no question of the wizard: two radio buttons.
static void yes_no(httpd_req_t *req, const char *name, text_id_t question, bool yes)
{
    send_str(req, "<label>");
    send_str(req, tr(question));
    send_str(req, "</label><label class=\"r\"><input type=\"radio\" name=\"");
    send_str(req, name);
    send_str(req, yes ? "\" value=\"1\" checked> " : "\" value=\"1\"> ");
    send_str(req, tr(T_WIZARD_YES));
    send_str(req, "</label><label class=\"r\"><input type=\"radio\" name=\"");
    send_str(req, name);
    send_str(req, yes ? "\" value=\"0\"> " : "\" value=\"0\" checked> ");
    send_str(req, tr(T_WIZARD_NO));
    send_str(req, "</label>");
}

// A checkbox that always sends its value: a hidden "0" and then "1" when it is on (the last value wins).
static void check_field(httpd_req_t *req, const char *name, text_id_t label, bool on)
{
    send_str(req, "<input type=\"hidden\" name=\"");
    send_str(req, name);
    send_str(req, "\" value=\"0\"><label class=\"r\"><input type=\"checkbox\" name=\"");
    send_str(req, name);
    send_str(req, on ? "\" value=\"1\" checked> " : "\" value=\"1\"> ");
    send_str(req, tr(label));
    send_str(req, "</label>");
}

// The sounds; the names are the list page.sounds.
static const char *const SOUNDS[] = { "siren", "warble", "beeps", "chime", "file", "off" };
_Static_assert(sizeof(SOUNDS) / sizeof(SOUNDS[0]) == T_PAGE_SOUNDS_ITEMS, "lang: page.sounds");

// The events in the order of web_sound_event_t: the settings; the names are the list page.events.
static const char *const EVENTS[] = { "snd_missile", "snd_drone", "snd_clear" };
_Static_assert(sizeof(EVENTS) / sizeof(EVENTS[0]) == T_PAGE_EVENTS_ITEMS, "lang: page.events");

static const char *event_sound(const settings_t *s, int event)
{
    return event == WEB_SOUND_MISSILE ? s->snd_missile : (event == WEB_SOUND_DRONE ? s->snd_drone : s->snd_clear);
}

static bool valid_sound(const char *id)
{
    return audio_sound_from_id(id, AUDIO_SOUND_COUNT) != AUDIO_SOUND_COUNT;
}

// The alert sounds: they apply at once (POST /live). The check buttons and the uploaded files.
static void sound_section(httpd_req_t *req, const settings_t *s)
{
    char buf[200];
    send_str(req, "<fieldset><legend>");
    send_str(req, tr(T_PAGE_SOUND_LEGEND));
    send_str(req, "</legend><form method=\"post\" action=\"/live\">"
                  "<input type=\"hidden\" name=\"alert_sound\" value=\"0\">"
                  "<label class=\"r\"><input type=\"checkbox\" name=\"alert_sound\" value=\"1\"");
    send_str(req, s->snd_alert[0] == '1' ? " checked> " : "> ");
    send_str(req, tr(T_PAGE_SOUND_ON));
    send_str(req, "</label><label>");
    send_str(req, tr(T_PAGE_VOLUME));
    send_str(req, "</label><input type=\"number\" name=\"volume\" min=\"0\" max=\"100\" step=\"5\" value=\"");
    send_escaped(req, s->snd_vol);
    send_str(req, "\">");
    for (int e = 0; e < (int)(sizeof(EVENTS) / sizeof(EVENTS[0])); e++) {
        audio_file_info_t info;
        audio_file_info(e, &info);
        snprintf(buf, sizeof(buf), "<label>%s</label><select name=\"%s\">", tra(T_PAGE_EVENTS, e), EVENTS[e]);
        send_str(req, buf);
        for (size_t i = 0; i < sizeof(SOUNDS) / sizeof(SOUNDS[0]); i++) {
            const bool file = strcmp(SOUNDS[i], "file") == 0;
            if (file && !info.valid && strcmp(event_sound(s, e), "file") != 0) {
                continue;   // no file: no choice "own file"
            }
            snprintf(buf, sizeof(buf), "<option value=\"%s\"%s>%s</option>", SOUNDS[i],
                     strcmp(event_sound(s, e), SOUNDS[i]) == 0 ? " selected" : "", tra(T_PAGE_SOUNDS, (int)i));
            send_str(req, buf);
        }
        send_str(req, "</select>");
    }
    // The check buttons are in the same form: the sound plays with the volume and the sounds of the form.
    // "Save" comes first, so Enter in a field saves and does not play.
    send_str(req, "<button type=\"submit\">");
    send_str(req, tr(T_PAGE_SOUND_SAVE));
    send_str(req, "</button><div class=\"scr\">");
    send_str(req, tr(T_PAGE_SOUND_CHECK));
    send_str(req, " <button name=\"check\" value=\"0\">");
    send_str(req, tr(T_PAGE_CHECK_MISSILE));
    send_str(req, "</button><button name=\"check\" value=\"1\">");
    send_str(req, tr(T_PAGE_CHECK_DRONE));
    send_str(req, "</button><button name=\"check\" value=\"2\">");
    send_str(req, tr(T_PAGE_CHECK_CLEAR));
    send_str(req, "</button></div></form><p><small>");
    send_str(req, tr(T_PAGE_SOUND_NOTE));
    send_str(req, "</small></p><h2>");
    send_str(req, tr(T_PAGE_OWN_SOUNDS));
    send_str(req, "</h2>");
    if (!audio_files_available()) {
        send_str(req, "<p><small>");
        send_str(req, tr(T_PAGE_OWN_SOUNDS_TABLE));
        send_str(req, "</small></p></fieldset>");
        return;
    }
    char note[400];
    snprintf(note, sizeof(note), tr(T_PAGE_OWN_SOUNDS_NOTE), audio_file_max_seconds(16000),
             audio_file_max_seconds(44100));
    send_str(req, "<p><small>");
    send_str(req, note);
    send_str(req, "</small></p>");
    for (int e = 0; e < (int)(sizeof(EVENTS) / sizeof(EVENTS[0])); e++) {
        audio_file_info_t info;
        audio_file_info(e, &info);
        snprintf(buf, sizeof(buf), "<label>%s: ", tra(T_PAGE_EVENTS, e));
        send_str(req, buf);
        if (info.valid) {
            send_str(req, "<b>");
            send_escaped(req, info.name[0] ? info.name : tr(T_PAGE_FILE));
            send_str(req, "</b>, ");
            snprintf(buf, sizeof(buf), tr(T_PAGE_FILE_INFO), info.tenths / 10, info.tenths % 10, info.rate);
            send_str(req, buf);
        } else {
            send_str(req, tr(T_PAGE_NO_FILE));
        }
        snprintf(buf, sizeof(buf),
                 "</label><input type=\"file\" id=\"f%d\" accept=\".wav,audio/wav\">"
                 "<button type=\"button\" onclick=\"up(%d)\">", e, e);
        send_str(req, buf);
        send_str(req, tr(T_PAGE_UPLOAD));
        send_str(req, "</button>");
        if (info.valid) {
            snprintf(buf, sizeof(buf), "<button type=\"button\" onclick=\"del(%d)\">", e);
            send_str(req, buf);
            send_str(req, tr(T_PAGE_DELETE_FILE));
            send_str(req, "</button>");
        }
    }
    send_str(req,
        "<p id=\"snd\"></p></fieldset>"
        "<script>function say(t){document.getElementById('snd').textContent=t;}"
        "function up(i){var f=document.getElementById('f'+i).files[0];if(!f){say(L.choose_wav);return;}"
        "var x=new XMLHttpRequest();x.open('POST','/sound/file?slot='+i+'&name='+encodeURIComponent(f.name));"
        "x.upload.onprogress=function(e){if(e.lengthComputable)"
        "say(L.sending+Math.round(e.loaded*100/e.total)+' %');};"
        "x.onload=function(){say(x.responseText);if(x.status==200)setTimeout(function(){location.reload();},1500);};"
        "x.onerror=function(){say(L.link_lost);};say(L.sending_now);x.send(f);}"
        "function del(i){var x=new XMLHttpRequest();x.open('POST','/sound/delete');"
        "x.onload=function(){location.reload();};x.send('slot='+i);}</script>");
}

// Eco mode: it applies at once (POST /live). Home Assistant has the same switch.
static void eco_section(httpd_req_t *req, const settings_t *s)
{
    send_str(req, "<fieldset><legend>");
    send_str(req, tr(T_PAGE_ECO_LEGEND));
    send_str(req, "</legend><form method=\"post\" action=\"/live\">"
                  "<input type=\"hidden\" name=\"eco\" value=\"0\">"
                  "<label class=\"r\"><input type=\"checkbox\" name=\"eco\" value=\"1\"");
    send_str(req, s->eco[0] == '1' ? " checked> " : "> ");
    send_str(req, tr(T_PAGE_ENABLED));
    send_str(req, "</label><label>");
    send_str(req, tr(T_PAGE_ECO_EVERY));
    send_str(req, "</label><input type=\"number\" name=\"eco_min\" min=\"5\" max=\"240\" step=\"5\" value=\"");
    send_escaped(req, s->eco_min);
    send_str(req, "\"><label>");
    send_str(req, tr(T_PAGE_SERVER_EVERY));
    send_str(req, "</label><input type=\"number\" name=\"srv_min\" min=\"1\" max=\"30\" step=\"1\" value=\"");
    send_escaped(req, s->srv_min);
    send_str(req, "\"><button type=\"submit\">");
    send_str(req, tr(T_PAGE_ECO_SAVE));
    send_str(req, "</button></form><p><small>");
    send_str(req, tr(T_PAGE_ECO_NOTE));
    send_str(req, "</small></p></fieldset>");
}

// The city of the forecast: it applies at once (POST /live). Home Assistant has the same list.
static void meteo_section(httpd_req_t *req, const settings_t *s)
{
    char buf[160];
    send_str(req, "<fieldset><legend>");
    send_str(req, tr(T_PAGE_METEO_LEGEND));
    send_str(req, "</legend><form method=\"post\" action=\"/live\"><label>");
    send_str(req, tr(T_PAGE_METEO_CITY));
    send_str(req, "</label><select name=\"meteo\">");
    snprintf(buf, sizeof(buf), "<option value=\"\"%s>%s</option>", s->meteo[0] ? "" : " selected", tr(T_METEO_NONE));
    send_str(req, buf);
    for (int i = 0; i < hadata_city_count(); i++) {
        snprintf(buf, sizeof(buf), "<option value=\"%s\"%s>%s</option>", hadata_city_station(i),
                 strcmp(s->meteo, hadata_city_station(i)) == 0 ? " selected" : "", hadata_city_name(i));
        send_str(req, buf);
    }
    // A station out of the list (an old setting) stays as it is until another choice.
    if (s->meteo[0] && hadata_city_find(s->meteo) < 0) {
        send_str(req, "<option value=\"");
        send_escaped(req, s->meteo);
        send_str(req, "\" selected>");
        snprintf(buf, sizeof(buf), tr(T_PAGE_METEO_STATION), s->meteo);
        send_escaped(req, buf);
        send_str(req, "</option>");
    }
    send_str(req, "</select><button type=\"submit\">");
    send_str(req, tr(T_PAGE_METEO_SAVE));
    send_str(req, "</button></form><p><small>");
    send_str(req, tr(T_PAGE_METEO_NOTE));
    send_str(req, "</small></p></fieldset>");
}

// The region of the alerts of ubilling.net.ua: it applies at once (POST /live). The form sends the number of the
// region: the keys are long UTF-8 texts. Home Assistant has the same list.
static void alerts_section(httpd_req_t *req, const settings_t *s)
{
    char buf[200];
    const int current = hadata_region_find(s->alert_reg);
    send_str(req, "<fieldset><legend>");
    send_str(req, tr(T_PAGE_ALERTS_LEGEND));
    send_str(req, "</legend><form method=\"post\" action=\"/live\"><label>");
    send_str(req, tr(T_PAGE_ALERTS_REGION));
    send_str(req, "</label><select name=\"alert_reg\">");
    snprintf(buf, sizeof(buf), "<option value=\"\"%s>%s</option>", current < 0 ? " selected" : "", tr(T_ALERTS_NONE));
    send_str(req, buf);
    for (int i = 0; i < hadata_region_count(); i++) {
        snprintf(buf, sizeof(buf), "<option value=\"%d\"%s>%s</option>", i, i == current ? " selected" : "",
                 hadata_region_name(i));
        send_str(req, buf);
    }
    send_str(req, "</select><button type=\"submit\">");
    send_str(req, tr(T_PAGE_ALERTS_SAVE));
    send_str(req, "</button></form><p><small>");
    send_str(req, tr(T_PAGE_ALERTS_NOTE));
    send_str(req, "</small></p></fieldset>");
}

// HTTPS with a certificate of the own CA: the key and the request (CSR) from the device, or a key and
// a certificate from a computer.
static void tls_section(httpd_req_t *req, const settings_t *s)
{
    char buf[400];
    char *about = malloc(320);
    send_str(req, "<fieldset><legend>");
    send_str(req, tr(T_PAGE_TLS_LEGEND));
    send_str(req, "</legend><p><small>");
    send_str(req, tr(T_PAGE_TLS_NOW));
    send_str(req, " ");
    if (about) {
        tlscert_describe(s->host, about, 320);
        send_escaped(req, about);
        free(about);
    }
    send_str(req, ". ");
    send_str(req, tr(T_PAGE_TLS_NAME));
    send_str(req, " <b>");
    send_escaped(req, s->host);
    send_str(req, "</b> ");
    send_str(req, tr(T_PAGE_TLS_NAME_NOTE));
    send_str(req, "</small></p><h2>");
    send_str(req, tr(T_PAGE_TLS_KEY_TITLE));
    send_str(req, "</h2><form method=\"post\" action=\"/tls/csr\"><button type=\"submit\">");
    send_str(req, tr(T_PAGE_TLS_MAKE_CSR));
    send_str(req, "</button></form><p><small>");
    // The device name has only a-z, 0-9, '-' and '.' (see valid_host()): no escape is necessary.
    snprintf(buf, sizeof(buf), tr(T_PAGE_TLS_CSR_NOTE), s->host);
    send_str(req, buf);
    send_str(req, "</small></p><label for=\"tc\">");
    send_str(req, tr(T_PAGE_TLS_CERT));
    send_str(req, "</label><input type=\"file\" id=\"tc\" accept=\".crt,.pem,.cer\">"
                  "<button type=\"button\" onclick=\"tls(['tc'])\">");
    send_str(req, tr(T_PAGE_TLS_INSTALL_CERT));
    send_str(req, "</button><h2>");
    send_str(req, tr(T_PAGE_TLS_OR_TITLE));
    send_str(req, "</h2><p><small>");
    snprintf(buf, sizeof(buf), tr(T_PAGE_TLS_ISSUE_NOTE), s->host);
    send_str(req, buf);
    send_str(req, "</small></p><label for=\"tk\">");
    send_str(req, tr(T_PAGE_TLS_KEY));
    send_str(req, "</label><input type=\"file\" id=\"tk\" accept=\".key,.pem\"><label for=\"tc2\">");
    send_str(req, tr(T_PAGE_TLS_CERT));
    send_str(req, "</label><input type=\"file\" id=\"tc2\" accept=\".crt,.pem,.cer\">"
                  "<button type=\"button\" onclick=\"tls(['tk','tc2'])\">");
    send_str(req, tr(T_PAGE_TLS_INSTALL_BOTH));
    send_str(req, "</button>");
    if (tlscert_installed()) {
        send_str(req, "<form method=\"post\" action=\"/tls/delete\" onsubmit=\"return confirm(L.tls_delete_confirm)\">"
                      "<button type=\"submit\">");
        send_str(req, tr(T_PAGE_TLS_DELETE));
        send_str(req, "</button></form>");
    }
    send_str(req, "<p><small>");
    send_str(req, tr(T_PAGE_TLS_NOTE));
    send_str(req, "</small></p>"
                  "<p id=\"tls\"></p></fieldset>"
                  "<script>function tls(ids){var s=document.getElementById('tls');"
                  "var fs=ids.map(function(i){return document.getElementById(i).files[0];});"
                  "if(fs.some(function(f){return !f;})){s.textContent=L.choose_files;return;}"
                  "Promise.all(fs.map(function(f){return f.text();})).then(function(t){"
                  "var x=new XMLHttpRequest();x.open('POST','/tls/cert');"
                  "x.onload=function(){s.textContent=x.responseText;"
                  "if(x.status==200)setTimeout(function(){location.href='https://'+location.hostname+'/';},12000);};"
                  "x.onerror=function(){s.textContent=L.link_lost;};x.send(t.join('\\n'));});}"
                  "</script>");
}

/* The page without the device */

// The text about a device that does not answer. st: the last known state, {screen, eco}. The main page and
// the offline page use it.
static const char STATE_TEXT_JS[] =
    "function text(st){"
    "if(st.screen=='clock')return L.state_clock;"
    "if(st.screen=='server')return L.state_server;"
    "if(st.eco)return L.state_eco;"
    "return L.state_none;}";

// The service worker (the browser runs it only on HTTPS). It keeps /offline.html. When the device does not answer
// a page request (the device sleeps) in 10 s, the browser shows that page instead of "This site can't be
// reached". The cache name has the firmware version: a new firmware gives a new service worker and a new copy.
static const char SW_JS[] =
    "self.addEventListener('install',function(e){self.skipWaiting();"
    "e.waitUntil(caches.open(CACHE).then(function(c){return c.add('/offline.html');}));});"
    "self.addEventListener('activate',function(e){e.waitUntil(caches.keys().then(function(k){"
    "return Promise.all(k.filter(function(n){return n!=CACHE;}).map(function(n){return caches.delete(n);}));"
    "}).then(function(){return self.clients.claim();}));});"
    "self.addEventListener('fetch',function(e){var r=e.request;"
    "if(r.mode!='navigate'||r.method!='GET')return;"
    "e.respondWith(new Promise(function(done){var over=false;"
    "function offline(){caches.match('/offline.html').then(function(c){done(c||Response.error());});}"
    "var t=setTimeout(function(){if(!over){over=true;offline();}},10000);"
    "fetch(r).then(function(res){if(!over){over=true;clearTimeout(t);done(res);}},"
    "function(){if(!over){over=true;clearTimeout(t);offline();}});}));});";

// GET /sw.js. No authorized(): it has no secrets, and it must not keep Wi-Fi on (see ping_get()).
static esp_err_t sw_get(httpd_req_t *req)
{
    char head[96];
    snprintf(head, sizeof(head), "var CACHE='rlcd-%s';", s_version ? s_version : "");
    begin_page(req);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    send_str(req, head);
    send_str(req, SW_JS);
    return end_page(req);
}

// GET /offline.html: the page for the service worker. It shows the last state that the main page kept
// in localStorage, asks /ping each 10 s and opens the real page when the device answers.
// No authorized(), as sw_get().
static esp_err_t offline_get(httpd_req_t *req)
{
    begin_page(req);
    send_head(req);
    send_str(req, "<p id=\"off\" class=\"off\"></p><p><small id=\"seen\"></small></p><p><small>");
    send_str(req, tr(T_PAGE_OFFLINE_REOPEN));
    send_str(req, "</small></p><script>");
    send_str(req, STATE_TEXT_JS);
    send_str(req, "var st={screen:'',eco:false,at:0};"
                  "try{st=JSON.parse(localStorage.getItem('rlcd'))||st;}catch(_){}"
                  "document.getElementById('off').textContent=text(st);"
                  "if(st.at)document.getElementById('seen').textContent=L.last_answer+"
                  "new Date(st.at).toLocaleString(L.locale);"
                  "setInterval(function(){fetch('/ping',{cache:'no-store'}).then(function(r){"
                  "if(r.ok)location.reload();}).catch(function(){});},10000);</script></body></html>");
    return end_page(req);
}

// GET /ping: the page asks it to know that the device answers. No authorized(): it records a page request,
// and a page request keeps Wi-Fi on for 2 minutes. Then an open page keeps the device awake in eco mode and on
// the server screen. The answer has no secrets: the screen and eco mode, for example
// {"screen":"server","eco":false}.
static esp_err_t ping_get(httpd_req_t *req)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"screen\":\"%s\",\"eco\":%s}", s_settings->screen,
             s_settings->eco[0] == '1' ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// The first-start wizard: the language, Wi-Fi, Home Assistant, trmnl-server, the city and the alert region.
// "Save" writes the answers (has_ha, has_srv) and restarts the device; then the full page opens.
static esp_err_t wizard_get(httpd_req_t *req)
{
    const settings_t *s = s_settings;
    char buf[200];
    begin_page(req);
    send_head(req);
    send_str(req, "<h2>");
    send_str(req, tr(T_WIZARD_TITLE));
    send_str(req, "</h2><p><small>");
    send_str(req, tr(T_WIZARD_INTRO));
    send_str(req, "</small></p><form method=\"post\" action=\"/save\"><fieldset><legend>");
    send_str(req, tr(T_PAGE_LANGUAGE));
    send_str(req, "</legend><select name=\"lang\">");
    for (int i = 0; i < TEXTS_LANGUAGE_COUNT; i++) {
        snprintf(buf, sizeof(buf), "<option value=\"%s\"%s>%s</option>", texts_language_code(i),
                 strcmp(texts_language_code(i), texts_language()) == 0 ? " selected" : "", texts_language_name(i));
        send_str(req, buf);
    }
    send_str(req, "</select></fieldset><fieldset><legend>Wi-Fi</legend>");
    text_field(req, "wifi_ssid", T_PAGE_WIFI_MAIN, s->wifi_ssid, 32, tr(T_PAGE_WIFI_NAME_HINT));
    password_field(req, "wifi_pass", T_PAGE_WIFI_MAIN_PASS, s->wifi_pass, 64);
    send_str(req, "</fieldset><fieldset><legend>Home Assistant</legend>");
    yes_no(req, "has_ha", T_WIZARD_HA_QUESTION, s->has_ha[0] != '0');
    text_field(req, "ha_url", T_PAGE_HA_URL, s->ha_url, 95, "http://homeassistant.local:8123");
    password_field(req, "ha_token", T_PAGE_HA_TOKEN, s->ha_token, 255);
    send_str(req, "<p><small>");
    send_str(req, tr(T_WIZARD_HA_NOTE));
    send_str(req, "</small></p><label>");
    send_str(req, tr(T_PAGE_ALERTS_REGION));
    send_str(req, "</label><select name=\"alert_reg\">");
    const int region = hadata_region_find(s->alert_reg);
    snprintf(buf, sizeof(buf), "<option value=\"\"%s>%s</option>", region < 0 ? " selected" : "", tr(T_ALERTS_NONE));
    send_str(req, buf);
    for (int i = 0; i < hadata_region_count(); i++) {
        snprintf(buf, sizeof(buf), "<option value=\"%d\"%s>%s</option>", i, i == region ? " selected" : "",
                 hadata_region_name(i));
        send_str(req, buf);
    }
    send_str(req, "</select></fieldset><fieldset><legend>trmnl-server</legend>");
    yes_no(req, "has_srv", T_WIZARD_SRV_QUESTION, s->has_srv[0] == '1');
    text_field(req, "srv_url", T_PAGE_SRV_URL, s->srv_url, 127, "http://…/quad.bmp");
    send_str(req, "<p><small>");
    send_str(req, tr(T_WIZARD_SRV_NOTE));
    send_str(req, "</small></p></fieldset><fieldset><legend>");
    send_str(req, tr(T_PAGE_METEO_LEGEND));
    send_str(req, "</legend><select name=\"meteo\">");
    snprintf(buf, sizeof(buf), "<option value=\"\"%s>%s</option>", s->meteo[0] ? "" : " selected", tr(T_METEO_NONE));
    send_str(req, buf);
    for (int i = 0; i < hadata_city_count(); i++) {
        snprintf(buf, sizeof(buf), "<option value=\"%s\"%s>%s</option>", hadata_city_station(i),
                 strcmp(s->meteo, hadata_city_station(i)) == 0 ? " selected" : "", hadata_city_name(i));
        send_str(req, buf);
    }
    send_str(req, "</select></fieldset><button type=\"submit\">");
    send_str(req, tr(T_PAGE_SAVE_RESTART));
    send_str(req, "</button></form><p><a href=\"/?all=1\">");
    send_str(req, tr(T_WIZARD_ALL));
    send_str(req, "</a></p></body></html>");
    return end_page(req);
}

static esp_err_t root_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    const settings_t *s = s_settings;
    // Before the first answers: the wizard. "/?all=1" opens the full page.
    char query[16] = "";
    if (!s->has_ha[0] && (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
                          strstr(query, "all=1") == NULL)) {
        return wizard_get(req);
    }
    begin_page(req);
    send_head(req);
    send_str(req, "<p id=\"off\" class=\"off\" hidden></p>");

    char *status = malloc(1536);
    if (status) {
        status[0] = '\0';
        if (s_status) {
            s_status(status, 1536);
        }
        send_str(req, "<h2>");
        send_str(req, tr(T_PAGE_DEVICE));
        send_str(req, "</h2><pre>");
        send_escaped(req, status);
        send_str(req, "</pre>");
        free(status);
    }

    // Screens: these forms change the screen at once, without a restart.
    send_str(req, "<fieldset><legend>");
    send_str(req, tr(T_PAGE_SCREEN_LEGEND));
    send_str(req, "</legend><form method=\"post\" action=\"/show\" class=\"scr\">");
    screen_buttons(req, s->screen);
    send_str(req, "</form><p><small>");
    send_str(req, tr(T_PAGE_SCREEN_NOTE));
    send_str(req, "</small></p><form method=\"post\" action=\"/note\"><label>");
    send_str(req, tr(T_PAGE_NOTE_LABEL));
    send_str(req, "</label><textarea name=\"note\" rows=\"4\" maxlength=\"250\" placeholder=\"");
    send_attr(req, T_PAGE_NOTE_PLACEHOLDER);
    send_str(req, "\">");
    send_escaped(req, s->note);
    send_str(req, "</textarea><button type=\"submit\">");
    send_str(req, tr(T_PAGE_NOTE_SHOW));
    send_str(req, "</button><p><small>");
    send_str(req, tr(T_PAGE_NOTE_HINT));
    send_str(req, "</small></p></form></fieldset>");

    sound_section(req, s);
    eco_section(req, s);
    meteo_section(req, s);
    alerts_section(req, s);

    // Settings: "Save" writes them and restarts the device.
    send_str(req, "<form method=\"post\" action=\"/save\"><fieldset><legend>Home Assistant</legend>");
    check_field(req, "has_ha", T_PAGE_HAS_HA, s->has_ha[0] != '0');
    text_field(req, "ha_url", T_PAGE_HA_URL, s->ha_url, 95, "http://homeassistant.local:8123");
    password_field(req, "ha_token", T_PAGE_HA_TOKEN, s->ha_token, 255);
    send_str(req, "<p><small>");
    send_str(req, tr(T_PAGE_ENTITIES_NOTE));
    send_str(req, "</small></p>");
    text_field(req, "e_temp", T_PAGE_E_TEMP, s->e_temp, 159, "sensor.…");
    text_field(req, "e_hum", T_PAGE_E_HUM, s->e_hum, 159, "sensor.…");
    text_field(req, "e_aqi", T_PAGE_E_AQI, s->e_aqi, 159, "sensor.…");
    text_field(req, "e_drone", T_PAGE_E_DRONE, s->e_drone, 95, "binary_sensor.…");
    text_field(req, "e_missile", T_PAGE_E_MISSILE, s->e_missile, 95, "binary_sensor.…");
    text_field(req, "e_ups", T_PAGE_E_UPS, s->e_ups, 95, "sensor.ups_status");
    text_field(req, "e_volt", T_PAGE_E_VOLT, s->e_volt, 95, "sensor.ups_input_voltage");
    text_field(req, "e_dtek", T_PAGE_E_DTEK, s->e_dtek, 95, tr(T_PAGE_E_DTEK_HINT));
    text_field(req, "e_off", T_PAGE_E_OFF, s->e_off, 95, "sensor.…_next_power_off");
    text_field(req, "e_on", T_PAGE_E_ON, s->e_on, 95, "sensor.…_next_power_on");
    text_field(req, "pw_queue", T_PAGE_PW_QUEUE, s->pw_queue, 47, tr(T_PAGE_PW_QUEUE_HINT));
    send_str(req, "</fieldset><fieldset><legend>");
    send_str(req, tr(T_PAGE_DTEK_LEGEND));
    send_str(req, "</legend>");
    password_field(req, "hc_key", T_PAGE_HC_KEY, s->hc_key, 63);
    text_field(req, "hc_check", T_PAGE_HC_CHECK, s->hc_check, 63, tr(T_PAGE_HC_CHECK_HINT));
    text_field(req, "ping_host", T_PAGE_PING_HOST, s->ping_host, 63, tr(T_PAGE_PING_HOST_HINT));
    send_str(req, "<p><small>");
    send_str(req, tr(T_PAGE_DTEK_NOTE));
    send_str(req, "</small></p></fieldset><fieldset><legend>");
    send_str(req, tr(T_PAGE_SERVER_LEGEND));
    send_str(req, "</legend>");
    check_field(req, "has_srv", T_PAGE_HAS_SRV, s->has_srv[0] != '0');
    text_field(req, "srv_url", T_PAGE_SRV_URL, s->srv_url, 127, "http://…/quad.bmp");
    send_str(req, "<p><small>");
    send_str(req, tr(T_PAGE_SERVER_NOTE));
    send_str(req, "</small></p></fieldset><fieldset><legend>Wi-Fi</legend>");
    text_field(req, "wifi_ssid", T_PAGE_WIFI_MAIN, s->wifi_ssid, 32, tr(T_PAGE_WIFI_NAME_HINT));
    password_field(req, "wifi_pass", T_PAGE_WIFI_MAIN_PASS, s->wifi_pass, 64);
    text_field(req, "wifi2_ssid", T_PAGE_WIFI_BACKUP, s->wifi2_ssid, 32, tr(T_PAGE_OPTIONAL));
    password_field(req, "wifi2_pass", T_PAGE_WIFI_BACKUP_PASS, s->wifi2_pass, 64);
    send_str(req, "</fieldset><fieldset><legend>DNS</legend>");
    text_field(req, "dns1", T_DNS_1, s->dns1, 15, tr(T_PAGE_DNS_HINT));
    text_field(req, "dns2", T_DNS_2, s->dns2, 15, tr(T_PAGE_OPTIONAL));
    send_str(req, "</fieldset><fieldset><legend>MQTT (Home Assistant)</legend>");
    text_field(req, "mqtt_uri", T_PAGE_MQTT_BROKER, s->mqtt_uri, 127, "mqtt://homeassistant.local:1883");
    text_field(req, "mqtt_user", T_PAGE_MQTT_USER, s->mqtt_user, 63, "");
    password_field(req, "mqtt_pass", T_PAGE_PASSWORD, s->mqtt_pass, 63);
    text_field(req, "mqtt_prefix", T_PAGE_MQTT_PREFIX, s->mqtt_prefix, 31, "homeassistant");
    send_str(req, "</fieldset><fieldset><legend>");
    send_str(req, tr(T_PAGE_PAGE_LEGEND));
    send_str(req, "</legend><label>");
    send_str(req, tr(T_PAGE_LANGUAGE));
    send_str(req, "</label><select name=\"lang\">");
    for (int i = 0; i < TEXTS_LANGUAGE_COUNT; i++) {
        char option[96];
        snprintf(option, sizeof(option), "<option value=\"%s\"%s>%s</option>", texts_language_code(i),
                 strcmp(texts_language_code(i), texts_language()) == 0 ? " selected" : "", texts_language_name(i));
        send_str(req, option);
    }
    send_str(req, "</select>");
    text_field(req, "host", T_PAGE_HOST, s->host, 63, "rlcd.local");
    password_field(req, "web_pass", T_PAGE_WEB_PASS, s->web_pass, 63);
    send_str(req, "<p><small>");
    send_str(req, tr(T_PAGE_WEB_PASS_NOTE));
    send_str(req, "</small></p></fieldset><button type=\"submit\">");
    send_str(req, tr(T_PAGE_SAVE_RESTART));
    send_str(req, "</button></form>");

    // Firmware update: outside the settings form. The scripts send the file or the URL as the request body.
    send_str(req, "<fieldset><legend>");
    send_str(req, tr(T_PAGE_OTA_LEGEND));
    send_str(req, "</legend><p><small>");
    send_str(req, tr(T_PAGE_OTA_NOTE));
    send_str(req, "</small></p><label for=\"fw\">");
    send_str(req, tr(T_PAGE_OTA_FROM_FILE));
    send_str(req, "</label><input type=\"file\" id=\"fw\" accept=\".tar\">"
                  "<button type=\"button\" onclick=\"upload()\">");
    send_str(req, tr(T_PAGE_OTA_UPDATE_FILE));
    send_str(req, "</button><label for=\"fwurl\">");
    send_str(req, tr(T_PAGE_OTA_URL));
    send_str(req, "</label><input type=\"url\" id=\"fwurl\" placeholder=\"");
    send_attr(req, T_PAGE_OTA_URL_HINT);
    send_str(req, "\" value=\"");
    send_escaped(req, s->ota_url);
    send_str(req, "\"><button type=\"button\" onclick=\"check()\">");
    send_str(req, tr(T_PAGE_OTA_CHECK));
    send_str(req, "</button><button type=\"button\" onclick=\"fromUrl()\">");
    send_str(req, tr(T_PAGE_OTA_DOWNLOAD));
    send_str(req,
        "</button><p id=\"ota\"></p></fieldset>"
        "<script>function show(t){document.getElementById('ota').textContent=t;}"
        "function upload(){var f=document.getElementById('fw').files[0];"
        "if(!f){show(L.choose_file);return;}"
        "var x=new XMLHttpRequest();x.open('POST','/ota');"
        "x.upload.onprogress=function(e){if(e.lengthComputable)"
        "show(L.sending+Math.round(e.loaded*100/e.total)+' %');};"
        "x.onload=function(){show(x.responseText);"
        "if(x.status==200)setTimeout(function(){location.reload();},25000);};"
        "x.onerror=function(){show(L.link_lost);};"
        "show(L.sending_now);x.send(f);}"
        "function poll(first){var x=new XMLHttpRequest();x.open('GET','/ota/status');"
        "x.onload=function(){if(x.status!=200)return;var j=JSON.parse(x.responseText);"
        "if(j.state=='running'){show(j.total?L.downloading+Math.round(j.done*100/j.total)+' %':"
        "(j.done?L.downloaded+Math.round(j.done/1024)+L.kb:j.text));setTimeout(poll,1000);}"
        "else if(first){}"
        "else if(j.state=='done'){show(j.text);setTimeout(function(){location.reload();},25000);}"
        "else if(j.state=='failed'){show(j.text);}};"
        "x.onerror=function(){if(!first)setTimeout(poll,2000);};x.send();}"
        "function fromUrl(){var u=document.getElementById('fwurl').value.trim();"
        "if(!u){show(L.need_url);return;}"
        "var x=new XMLHttpRequest();x.open('POST','/ota/url');"
        "x.onload=function(){show(x.responseText);if(x.status==202)setTimeout(poll,1000);};"
        "x.onerror=function(){show(L.link_lost);};show(L.starting);x.send(u);}"
        "function check(){var u=document.getElementById('fwurl').value.trim();"
        "if(!u){show(L.need_url);return;}"
        "var x=new XMLHttpRequest();x.open('POST','/ota/check');x.timeout=40000;"
        "x.onload=function(){var r;try{r=JSON.parse(x.responseText);}catch(_){show(x.responseText);return;}"
        "show(r.text);if(r.offer&&confirm(r.text))fromUrl();};"
        "x.onerror=x.ontimeout=function(){show(L.link_lost);};show(L.checking);x.send(u);}"
        "poll(true);</script>");

    tls_section(req, s);

    char buf[320];
    send_str(req, "<p><small>");
    snprintf(buf, sizeof(buf), tr(T_PAGE_FOOTER), s_version, net_ap_ssid(), net_ap_password(), net_ap_ip());
    send_str(req, buf);
    send_str(req, "</small></p>");

    // Each 10 s the page asks /ping. It does not keep Wi-Fi on (see ping_get()). After two requests without
    // an answer, the banner at the top tells why the device does not answer. The last state goes to
    // localStorage for the offline page of the service worker.
    snprintf(buf, sizeof(buf), "<script>var st={screen:'%s',eco:%s};", s->screen, s->eco[0] == '1' ? "true" : "false");
    send_str(req, buf);
    send_str(req, STATE_TEXT_JS);
    send_str(req, "(function(){var e=document.getElementById('off'),bad=0;"
                  "function keep(){st.at=Date.now();try{localStorage.setItem('rlcd',JSON.stringify(st));}catch(_){}}"
                  "keep();"
                  "if('serviceWorker' in navigator)navigator.serviceWorker.register('/sw.js').catch(function(){});"
                  "function ping(){var x=new XMLHttpRequest();x.open('GET','/ping');x.timeout=6000;"
                  "x.onload=function(){bad=0;e.hidden=true;"
                  "try{var j=JSON.parse(x.responseText);st.screen=j.screen;st.eco=j.eco;}catch(_){}keep();};"
                  "x.onerror=x.ontimeout=function(){if(++bad>=2){e.textContent=text(st);e.hidden=false;}};"
                  "x.send();}"
                  "setInterval(ping,10000);})();</script></body></html>");
    return end_page(req);
}

/* Save */

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

// A device name: labels of a-z, 0-9 and '-', joined with dots, for example "rlcd.lan". Make it lower case.
static bool valid_host(char *s)
{
    size_t label = 0;
    if (s[0] == '\0' || s[0] == '.' || s[0] == '-') {
        return false;
    }
    for (char *p = s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
        if (*p == '.') {
            if (label == 0) {
                return false;
            }
            label = 0;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-') {
            if (++label > 63) {
                return false;
            }
        } else {
            return false;
        }
    }
    return label > 0;
}

static bool valid_ip_or_empty(const char *s)
{
    esp_ip4_addr_t ip;
    return s[0] == '\0' || esp_netif_str_to_ip4(s, &ip) == ESP_OK;
}

static esp_err_t send_message(httpd_req_t *req, const char *status, const char *text, int refresh)
{
    httpd_resp_set_status(req, status);
    begin_page(req);
    send_head(req);
    if (refresh > 0) {
        char buf[80];
        snprintf(buf, sizeof(buf), "<meta http-equiv=\"refresh\" content=\"%d;url=/\">", refresh);
        send_str(req, buf);
    }
    send_str(req, "<p>");
    send_str(req, text);
    send_str(req, "</p><p><a href=\"/\">");
    send_str(req, tr(T_PAGE_BACK));
    send_str(req, "</a></p></body></html>");
    return end_page(req);
}

// Copy a form value to a settings field. A password field keeps the old value when the form value is empty.
static void apply_field(settings_t *s, const char *key, const char *value)
{
    struct {
        const char *key;
        char *dst;
        size_t size;
        bool password;
    } fields[] = {
        { "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid), false },
        { "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass), true },
        { "wifi2_ssid", s->wifi2_ssid, sizeof(s->wifi2_ssid), false },
        { "wifi2_pass", s->wifi2_pass, sizeof(s->wifi2_pass), true },
        { "dns1", s->dns1, sizeof(s->dns1), false },
        { "dns2", s->dns2, sizeof(s->dns2), false },
        { "mqtt_uri", s->mqtt_uri, sizeof(s->mqtt_uri), false },
        { "mqtt_user", s->mqtt_user, sizeof(s->mqtt_user), false },
        { "mqtt_pass", s->mqtt_pass, sizeof(s->mqtt_pass), true },
        { "mqtt_prefix", s->mqtt_prefix, sizeof(s->mqtt_prefix), false },
        { "web_pass", s->web_pass, sizeof(s->web_pass), true },
        { "screen", s->screen, sizeof(s->screen), false },
        { "ha_url", s->ha_url, sizeof(s->ha_url), false },
        { "ha_token", s->ha_token, sizeof(s->ha_token), true },
        { "e_temp", s->e_temp, sizeof(s->e_temp), false },
        { "e_hum", s->e_hum, sizeof(s->e_hum), false },
        { "e_aqi", s->e_aqi, sizeof(s->e_aqi), false },
        { "e_volt", s->e_volt, sizeof(s->e_volt), false },
        { "e_ups", s->e_ups, sizeof(s->e_ups), false },
        { "e_drone", s->e_drone, sizeof(s->e_drone), false },
        { "e_missile", s->e_missile, sizeof(s->e_missile), false },
        { "e_off", s->e_off, sizeof(s->e_off), false },
        { "e_on", s->e_on, sizeof(s->e_on), false },
        { "e_dtek", s->e_dtek, sizeof(s->e_dtek), false },
        { "hc_key", s->hc_key, sizeof(s->hc_key), true },
        { "hc_check", s->hc_check, sizeof(s->hc_check), false },
        { "ping_host", s->ping_host, sizeof(s->ping_host), false },
        { "pw_queue", s->pw_queue, sizeof(s->pw_queue), false },
        { "srv_url", s->srv_url, sizeof(s->srv_url), false },
        { "host", s->host, sizeof(s->host), false },
        { "lang", s->lang, sizeof(s->lang), false },
        { "has_ha", s->has_ha, sizeof(s->has_ha), false },
        { "has_srv", s->has_srv, sizeof(s->has_srv), false },
        { "meteo", s->meteo, sizeof(s->meteo), false },
    };
    if (strcmp(key, "note") == 0) {
        copy_note(s->note, value, sizeof(s->note));
        return;
    }
    if (strcmp(key, "alert_reg") == 0) {
        // The wizard sends the number of the region, "" = none.
        strlcpy(s->alert_reg, value[0] ? hadata_region_key(atoi(value)) : "", sizeof(s->alert_reg));
        return;
    }
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const size_t klen = strlen(fields[i].key);
        if (strcmp(key, fields[i].key) == 0) {
            if (!fields[i].password || value[0] != '\0') {
                strlcpy(fields[i].dst, value, fields[i].size);
            }
            return;
        }
        if (fields[i].password && strncmp(key, fields[i].key, klen) == 0 && strcmp(key + klen, "_clear") == 0) {
            fields[i].dst[0] = '\0';
            return;
        }
    }
}

/* Screen buttons and the note: no restart */

// Read a small form body. Return a buffer to free, or NULL.
static char *read_body(httpd_req_t *req)
{
    if (req->content_len >= MAX_BODY) {
        return NULL;
    }
    char *body = calloc(1, MAX_BODY);
    if (!body) {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += (size_t)n;
    }
    return body;
}

// Find a field in a form body and decode it in place. Return NULL if the body has no such field.
static char *form_value(char *body, const char *key)
{
    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        url_decode(pair);
        if (strcmp(pair, key) == 0) {
            url_decode(eq + 1);
            return eq + 1;
        }
    }
    return NULL;
}

// Show the settings page again. The main task changes the screen in some milliseconds.
static esp_err_t back_to_page(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t show_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char *body = read_body(req);
    if (!body) {
        return ESP_FAIL;
    }
    const char *id = form_value(body, "screen");
    if (id && valid_screen(id) && s_on_show) {
        ESP_LOGI(TAG, "Show the screen \"%s\"", id);
        s_on_show(id, NULL);
    }
    free(body);
    return back_to_page(req);
}

static esp_err_t note_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char *body = read_body(req);
    if (!body) {
        return ESP_FAIL;
    }
    const char *text = form_value(body, "note");
    if (text && s_on_show) {
        char note[sizeof(((settings_t *)0)->note)];
        copy_note(note, text, sizeof(note));
        ESP_LOGI(TAG, "Show a new note (%u bytes)", (unsigned)strlen(note));
        s_on_show("note", note);
    }
    free(body);
    return back_to_page(req);
}

static bool valid_number(const char *s, long min, long max)
{
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    return end != s && *end == '\0' && v >= min && v <= max;
}

// Check a value of a setting that applies at once.
static bool valid_live(const char *key, const char *value)
{
    if (strcmp(key, "eco") == 0 || strcmp(key, "alert_sound") == 0) {
        return strcmp(value, "0") == 0 || strcmp(value, "1") == 0;
    }
    if (strcmp(key, "eco_min") == 0) {
        return valid_number(value, 5, 240);
    }
    if (strcmp(key, "srv_min") == 0) {
        return valid_number(value, 1, 30);
    }
    if (strcmp(key, "volume") == 0) {
        return valid_number(value, 0, 100);
    }
    if (strcmp(key, "meteo") == 0) {
        return value[0] == '\0' || hadata_city_find(value) >= 0;
    }
    if (strcmp(key, "alert_reg") == 0) {
        return value[0] == '\0' || valid_number(value, 0, hadata_region_count() - 1);
    }
    return valid_sound(value);
}

// Settings that apply at once, without a restart (sound and eco mode). For a checkbox the form sends
// a hidden "0" and then "1" when it is on: the last value wins. A sound check button sends also
// check=0 (missile), 1 (drone) or 2 (all clear): the sound plays after the settings apply.
// The answer comes after the device applies them, so the next page shows the new values.
static esp_err_t live_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char *body = read_body(req);
    if (!body) {
        return ESP_FAIL;
    }
    static const char *const KEYS[] = { "eco", "eco_min", "srv_min", "alert_sound", "volume", "snd_missile", "snd_drone",
                                        "snd_clear", "meteo", "alert_reg" };
    enum { KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]) };
    char values[KEY_COUNT][16] = { { 0 } };
    bool have[KEY_COUNT] = { false };
    int check = -1;
    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        url_decode(pair);
        url_decode(eq + 1);
        if (strcmp(pair, "check") == 0 && valid_number(eq + 1, WEB_SOUND_MISSILE, WEB_SOUND_CLEAR)) {
            check = atoi(eq + 1);
        }
        for (int i = 0; i < KEY_COUNT; i++) {
            if (strcmp(pair, KEYS[i]) == 0) {
                strlcpy(values[i], eq + 1, sizeof(values[i]));
                have[i] = true;
            }
        }
    }
    free(body);
    for (int i = 0; i < KEY_COUNT; i++) {
        if (have[i] && valid_live(KEYS[i], values[i]) && s_on_set) {
            s_on_set(KEYS[i], values[i]);
        }
    }
    if (check >= 0 && s_on_sound) {
        ESP_LOGI(TAG, "Sound check: %s", EVENTS[check]);
        s_on_sound((web_sound_event_t)check);
    }
    if (s_wait_applied) {
        s_wait_applied();
    }
    return back_to_page(req);
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    if (req->content_len >= MAX_BODY) {
        return send_message(req, "413 Payload Too Large", tr(T_MSG_TOO_MUCH), 0);
    }
    char *body = calloc(1, MAX_BODY);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += (size_t)n;
    }

    settings_t next = *s_settings;
    // A password "_clear" checkbox comes after its field, so it wins.
    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        url_decode(pair);
        url_decode(eq + 1);
        apply_field(&next, pair, eq + 1);
    }
    free(body);

    if (!valid_ip_or_empty(next.dns1) || !valid_ip_or_empty(next.dns2)) {
        return send_message(req, "400 Bad Request", tr(T_MSG_BAD_DNS), 0);
    }
    if (next.mqtt_uri[0] && strncmp(next.mqtt_uri, "mqtt://", 7) != 0 && strncmp(next.mqtt_uri, "mqtts://", 8) != 0) {
        return send_message(req, "400 Bad Request", tr(T_MSG_BAD_MQTT), 0);
    }
    if (next.ha_url[0] && strncmp(next.ha_url, "http://", 7) != 0 && strncmp(next.ha_url, "https://", 8) != 0) {
        return send_message(req, "400 Bad Request", tr(T_MSG_BAD_HA), 0);
    }
    if (!valid_host(next.host)) {
        return send_message(req, "400 Bad Request",
                            tr(T_MSG_BAD_HOST), 0);
    }
    if (next.has_ha[0] != '0' && next.has_ha[0] != '1') {
        strlcpy(next.has_ha, "1", sizeof(next.has_ha));
    }
    if (next.has_srv[0] != '0' && next.has_srv[0] != '1') {
        strlcpy(next.has_srv, next.srv_url[0] ? "1" : "0", sizeof(next.has_srv));
    }
    if (next.meteo[0] && hadata_city_find(next.meteo) < 0 && strcmp(next.meteo, s_settings->meteo) != 0) {
        next.meteo[0] = '\0';
    }
    if (!settings_screen_available(&next, next.screen)) {
        strlcpy(next.screen, "room", sizeof(next.screen));
    }
    if (settings_save(&next) != ESP_OK) {
        return send_message(req, "500 Internal Server Error", tr(T_MSG_SAVE_FAILED), 0);
    }
    *s_settings = next;
    ESP_LOGI(TAG, "New settings are saved. Restart.");

    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 1500 * 1000);
    }
    return send_message(req, "200 OK", tr(T_MSG_SAVED), 15);
}

// Send all unknown addresses to the settings page. Phones open it as a captive portal.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS needs content in the answer to show the captive portal.
    return httpd_resp_sendstr(req, "Redirect to the settings page");
}

/* Firmware update (OTA) */

#define OTA_CHUNK       4096
#define OTA_MAX_ARCHIVE (8 * 1024 * 1024)    // more than an app slot and the archive headers
#define OTA_MAX_WAITS   5       // receive timeouts in a row before the update stops

static void notify(void)
{
    if (s_on_change) {
        s_on_change();
    }
}

static esp_err_t ota_reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

// Receive up to len bytes. Return the count, or -1 when the connection stops.
static int receive(httpd_req_t *req, uint8_t *buf, size_t len)
{
    for (int waits = 0; waits < OTA_MAX_WAITS; waits++) {
        const int n = httpd_req_recv(req, (char *)buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return n > 0 ? n : -1;
        }
    }
    return -1;
}

static void restart_later(void)
{
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 1500 * 1000);
    }
}

// The browser sends the release archive as the request body. fwupdate checks the archive, writes
// the image to the other app slot and makes that slot the boot slot. The new firmware must confirm
// itself after the start (see main), else the bootloader goes back to this firmware.
static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    if (req->content_len == 0 || req->content_len > OTA_MAX_ARCHIVE) {
        return ota_reply(req, "400 Bad Request", tr(T_MSG_OTA_BAD_SIZE));
    }
    uint8_t *buf = malloc(OTA_CHUNK);
    if (!buf) {
        return ota_reply(req, "500 Internal Server Error", tr(T_MSG_NO_MEMORY));
    }
    fwupdate_t *u = fwupdate_begin();
    if (!u) {
        free(buf);
        return ota_reply(req, "409 Conflict", fwupdate_begin_error());
    }
    ESP_LOGI(TAG, "OTA: receive an archive of %u bytes", (unsigned)req->content_len);

    size_t got = 0;
    bool lost = false;
    esp_err_t err = ESP_OK;
    while (got < req->content_len) {
        const size_t want = req->content_len - got < OTA_CHUNK ? req->content_len - got : OTA_CHUNK;
        const int n = receive(req, buf, want);
        if (n < 0) {
            lost = true;
            break;
        }
        got += (size_t)n;
        err = fwupdate_write(u, buf, (size_t)n);
        if (err != ESP_OK) {
            break;
        }
    }
    free(buf);
    if (lost) {
        ESP_LOGW(TAG, "OTA: the connection stopped");
        fwupdate_end(u);
        return ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = fwupdate_finish(u);
    }
    char text[256];
    if (err != ESP_OK) {
        strlcpy(text, fwupdate_error(u), sizeof(text));
        fwupdate_end(u);
        return ota_reply(req, "400 Bad Request", text);
    }
    snprintf(text, sizeof(text), tr(T_MSG_OTA_DONE), fwupdate_version(u), fwupdate_slot(u));
    fwupdate_end(u);
    restart_later();
    return ota_reply(req, "200 OK", text);
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The page sends the archive URL as the request body. The download runs in a separate task,
// and the page reads the progress from /ota/status. The device keeps the URL for the next time.
// Read the URL of an archive from the request body (http:// or https://, without spaces at the ends) and keep
// it in the settings. Return the URL, or NULL with the answer in *answer.
static char *read_archive_url(httpd_req_t *req, char *url, size_t size, esp_err_t *answer)
{
    if (req->content_len == 0 || req->content_len >= size) {
        *answer = ota_reply(req, "400 Bad Request", tr(T_MSG_URL_BAD_LENGTH));
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = receive(req, (uint8_t *)url + got, req->content_len - got);
        if (n < 0) {
            *answer = ESP_FAIL;
            return NULL;
        }
        got += (size_t)n;
    }
    url[got] = '\0';
    char *start = url;
    while (is_space(*start)) {
        start++;
    }
    size_t len = strlen(start);
    while (len > 0 && is_space(start[len - 1])) {
        start[--len] = '\0';
    }
    if (strncmp(start, "http://", 7) != 0 && strncmp(start, "https://", 8) != 0) {
        *answer = ota_reply(req, "400 Bad Request", tr(T_MSG_URL_BAD_SCHEME));
        return NULL;
    }
    if (strcmp(s_settings->ota_url, start) != 0) {
        strlcpy(s_settings->ota_url, start, sizeof(s_settings->ota_url));
        settings_save_field("ota_url", start);
    }
    return start;
}

static esp_err_t ota_url_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char url[sizeof(s_settings->ota_url)];
    esp_err_t answer = ESP_OK;
    const char *start = read_archive_url(req, url, sizeof(url), &answer);
    if (!start) {
        return answer;
    }
    const esp_err_t err = fwupdate_download(start);
    if (err == ESP_ERR_INVALID_STATE) {
        return ota_reply(req, "409 Conflict", tr(T_MSG_OTA_RUNNING));
    }
    if (err != ESP_OK) {
        return ota_reply(req, "500 Internal Server Error", tr(T_MSG_OTA_START_FAILED));
    }
    return ota_reply(req, "202 Accepted", tr(T_MSG_OTA_STARTED));
}

// Copy a text into a JSON string: escape '"' and '\', change control characters to spaces.
static void json_escape(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; *in && n + 2 < size; in++) {
        if (*in == '"' || *in == '\\') {
            out[n++] = '\\';
            out[n++] = *in;
        } else {
            out[n++] = (unsigned char)*in < 0x20 ? ' ' : *in;
        }
    }
    out[n] = '\0';
}

// Compare versions like "0.1.10" and "0.2" number by number. Return <0, 0 or >0.
static int compare_versions(const char *a, const char *b)
{
    for (;;) {
        char *ea, *eb;
        const long na = strtol(a, &ea, 10);
        const long nb = strtol(b, &eb, 10);
        if (ea == a || eb == b) {
            return strcmp(a, b);    // not a number: compare the text
        }
        if (na != nb) {
            return na < nb ? -1 : 1;
        }
        if (*ea != '.' || *eb != '.') {
            return strcmp(ea, eb);  // the end, or a suffix
        }
        a = ea + 1;
        b = eb + 1;
    }
}

// POST /ota/check: the body is the URL of an archive. Read only its manifest and compare it with the running
// firmware. The answer: {"offer": true if an update makes sense, "text": "..."}; the page asks before it.
static esp_err_t ota_check_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char url[sizeof(s_settings->ota_url)];
    esp_err_t answer = ESP_OK;
    const char *start = read_archive_url(req, url, sizeof(url), &answer);
    if (!start) {
        return answer;
    }
    fwupdate_manifest_t *m = calloc(1, sizeof(*m));
    char *text = malloc(320);
    char *json = malloc(720);
    if (!m || !text || !json) {
        free(m);
        free(text);
        free(json);
        return ota_reply(req, "500 Internal Server Error", tr(T_MSG_NO_MEMORY));
    }
    bool offer = false;
    if (fwupdate_check(start, m, text, 320) == ESP_OK) {
        const esp_app_desc_t *app = esp_app_get_description();
        char mine[72] = "";
        esp_app_get_elf_sha256(mine, sizeof(mine));
        char server[160];
        char built[64] = "";
        if (m->built[0]) {
            snprintf(built, sizeof(built), tr(T_MSG_BUILT), m->built);
        }
        snprintf(server, sizeof(server), "%s%s%s%s%s", m->version, m->git[0] ? " (" : "", m->git,
                 m->git[0] ? ")" : "", built);
        if (strcmp(m->project, app->project_name) != 0) {
            snprintf(text, 320, tr(T_MSG_OTHER_PROJECT), m->project);
        } else if (strcmp(m->chip, CONFIG_IDF_TARGET) != 0) {
            snprintf(text, 320, tr(T_MSG_OTHER_CHIP), m->chip);
        } else if (m->elf_sha256[0] && strcasecmp(m->elf_sha256, mine) == 0) {
            snprintf(text, 320, tr(T_MSG_NO_UPDATE), server);
        } else {
            const int cmp = compare_versions(m->version, app->version);
            offer = true;
            if (cmp > 0) {
                snprintf(text, 320, tr(T_MSG_NEWER), server, s_version);
            } else if (cmp < 0) {
                snprintf(text, 320, tr(T_MSG_OLDER), server, s_version);
            } else {
                snprintf(text, 320, tr(T_MSG_SAME_VERSION), server, s_version);
            }
        }
    }
    char quoted[640];
    json_escape(quoted, sizeof(quoted), text);
    snprintf(json, 720, "{\"offer\":%s,\"text\":\"%s\"}", offer ? "true" : "false", quoted);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    free(m);
    free(text);
    free(json);
    return err;
}

// The state of the download from a URL, for the page script.
static esp_err_t ota_status_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    static const char *const STATES[] = { "idle", "running", "done", "failed" };
    fwupdate_status_t st;
    fwupdate_get_status(&st);
    char text[2 * sizeof(st.text)];
    json_escape(text, sizeof(text), st.text);
    char out[sizeof(text) + 96];
    snprintf(out, sizeof(out), "{\"state\":\"%s\",\"done\":%u,\"total\":%u,\"text\":\"%s\"}",
             st.state <= FWUPDATE_FAILED ? STATES[st.state] : "idle", (unsigned)st.done, (unsigned)st.total, text);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

bool web_ota_active(void)
{
    return fwupdate_active();
}

int64_t web_last_request_us(void)
{
    return s_last_request_us;
}

/* Uploaded sounds */

#define SOUND_MAX_FILE (16 * 1024 * 1024)

// The browser sends a WAV file as the request body: /sound/file?slot=0&name=<file name>.
static esp_err_t sound_file_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char query[512] = "", value[300] = "", name[64] = "";
    int slot = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "slot", value, sizeof(value)) == ESP_OK) {
            slot = atoi(value);
        }
        if (httpd_query_key_value(query, "name", value, sizeof(value)) == ESP_OK) {
            url_decode(value);
            strlcpy(name, value, sizeof(name));
        }
    }
    if (req->content_len == 0 || req->content_len > SOUND_MAX_FILE) {
        return ota_reply(req, "400 Bad Request", tr(T_MSG_BAD_FILE_SIZE));
    }
    uint8_t *buf = malloc(OTA_CHUNK);
    if (!buf) {
        return ota_reply(req, "500 Internal Server Error", tr(T_MSG_NO_MEMORY));
    }
    audio_upload_t *u = audio_upload_begin(slot, name);
    if (!u) {
        free(buf);
        return ota_reply(req, "409 Conflict", audio_upload_begin_error());
    }
    size_t got = 0;
    bool lost = false;
    esp_err_t err = ESP_OK;
    while (got < req->content_len) {
        const size_t want = req->content_len - got < OTA_CHUNK ? req->content_len - got : OTA_CHUNK;
        const int n = receive(req, buf, want);
        if (n < 0) {
            lost = true;
            break;
        }
        got += (size_t)n;
        err = audio_upload_write(u, buf, (size_t)n);
        if (err != ESP_OK) {
            break;
        }
    }
    free(buf);
    if (lost) {
        audio_upload_end(u);
        return ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = audio_upload_finish(u);
    }
    char text[200];
    if (err != ESP_OK) {
        strlcpy(text, audio_upload_error(u), sizeof(text));
        audio_upload_end(u);
        return ota_reply(req, "400 Bad Request", text);
    }
    audio_upload_end(u);
    audio_file_info_t info;
    audio_file_info(slot, &info);
    snprintf(text, sizeof(text), tr(T_MSG_SOUND_SAVED), info.tenths / 10, info.tenths % 10);
    return ota_reply(req, "200 OK", text);
}

static esp_err_t sound_delete_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char *body = read_body(req);
    if (!body) {
        return ESP_FAIL;
    }
    const char *value = form_value(body, "slot");
    const int slot = value ? atoi(value) : -1;
    free(body);
    if (audio_file_delete(slot) != ESP_OK) {
        return ota_reply(req, "409 Conflict", tr(T_MSG_SOUND_DELETE_FAILED));
    }
    return ota_reply(req, "200 OK", tr(T_MSG_SOUND_DELETED));
}

/* HTTPS certificate */

// Make a new key and a certificate request (CSR). The browser saves the request as a file.
static esp_err_t tls_csr_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    free(read_body(req));
    char *csr = malloc(2048);
    if (!csr) {
        return ota_reply(req, "500 Internal Server Error", tr(T_MSG_NO_MEMORY));
    }
    if (tlscert_make_request(s_settings->host, csr, 2048) != ESP_OK) {
        free(csr);
        return ota_reply(req, "500 Internal Server Error", tr(T_MSG_CSR_FAILED));
    }
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s.csr\"", s_settings->host);
    httpd_resp_set_type(req, "application/pkcs10");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    const esp_err_t err = httpd_resp_sendstr(req, csr);
    free(csr);
    return err;
}

// The page sends PEM text: a certificate, or a key and a certificate. After the install the device restarts
// with the HTTPS server.
static esp_err_t tls_cert_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    if (req->content_len == 0 || req->content_len > TLSCERT_PEM_MAX) {
        return ota_reply(req, "400 Bad Request", tr(T_MSG_CERT_BAD_SIZE));
    }
    char *pem = calloc(1, req->content_len + 1);
    if (!pem) {
        return ota_reply(req, "500 Internal Server Error", tr(T_MSG_NO_MEMORY));
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = receive(req, (uint8_t *)pem + got, req->content_len - got);
        if (n < 0) {
            free(pem);
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    char msg[300];
    const esp_err_t err = tlscert_install(s_settings->host, pem, msg, sizeof(msg));
    memset(pem, 0, req->content_len);
    free(pem);
    if (err != ESP_OK) {
        return ota_reply(req, "400 Bad Request", msg);
    }
    restart_later();
    return ota_reply(req, "200 OK", msg);
}

static esp_err_t tls_delete_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    free(read_body(req));
    tlscert_delete();
    restart_later();
    return send_message(req, "200 OK", tr(T_MSG_CERT_DELETED), 15);
}

// The device has no icon. Answer "no content", so that the browser stops and the log has no warning.
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_start(const web_config_t *config)
{
    s_on_change = config->on_change;
    s_on_show = config->on_show;
    s_on_set = config->on_set;
    s_on_sound = config->on_sound;
    s_wait_applied = config->wait_applied;
    s_settings = config->settings;
    s_version = config->version;
    s_status = config->status;
    fwupdate_set_notify(notify);

    static httpd_uri_t HANDLERS[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/ping", .method = HTTP_GET, .handler = ping_get },
        { .uri = "/sw.js", .method = HTTP_GET, .handler = sw_get },
        { .uri = "/offline.html", .method = HTTP_GET, .handler = offline_get },
        { .uri = "/save", .method = HTTP_POST, .handler = save_post },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get },
        { .uri = "/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/ota/url", .method = HTTP_POST, .handler = ota_url_post },
        { .uri = "/ota/check", .method = HTTP_POST, .handler = ota_check_post },
        { .uri = "/ota/status", .method = HTTP_GET, .handler = ota_status_get },
        { .uri = "/show", .method = HTTP_POST, .handler = show_post },
        { .uri = "/note", .method = HTTP_POST, .handler = note_post },
        { .uri = "/live", .method = HTTP_POST, .handler = live_post },
        { .uri = "/sound/file", .method = HTTP_POST, .handler = sound_file_post },
        { .uri = "/sound/delete", .method = HTTP_POST, .handler = sound_delete_post },
        { .uri = "/tls/csr", .method = HTTP_POST, .handler = tls_csr_post },
        { .uri = "/tls/cert", .method = HTTP_POST, .handler = tls_cert_post },
        { .uri = "/tls/delete", .method = HTTP_POST, .handler = tls_delete_post },
    };
    const size_t count = sizeof(HANDLERS) / sizeof(HANDLERS[0]);

    // HTTPS first: the HTTP handlers check s_https.
    if (tlscert_installed() && tlscert_load(&s_tls_cert, &s_tls_key) == ESP_OK) {
        httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
        ssl.servercert = (const uint8_t *)s_tls_cert;
        ssl.servercert_len = strlen(s_tls_cert) + 1;
        ssl.prvtkey_pem = (const uint8_t *)s_tls_key;
        ssl.prvtkey_len = strlen(s_tls_key) + 1;
        ssl.httpd.max_open_sockets = 3;
        ssl.httpd.max_uri_handlers = count + 4;
        ssl.httpd.lru_purge_enable = true;
        ssl.httpd.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;   // the HTTP server has the default one
        httpd_handle_t secure = NULL;
        const esp_err_t err = httpd_ssl_start(&secure, &ssl);
        if (err == ESP_OK) {
            for (size_t i = 0; i < count; i++) {
                httpd_uri_t h = HANDLERS[i];
                h.user_ctx = (void *)1;
                httpd_register_uri_handler(secure, &h);
            }
            httpd_register_err_handler(secure, HTTPD_404_NOT_FOUND, not_found);
            s_https = true;
            ESP_LOGI(TAG, "HTTPS settings page is on port %d", ssl.port_secure);
        } else {
            // Without HTTPS the page stays on HTTP: the device must stay reachable.
            ESP_LOGE(TAG, "Cannot start the HTTPS server: %s", esp_err_to_name(err));
            free(s_tls_cert);
            free(s_tls_key);
            s_tls_cert = s_tls_key = NULL;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = count + 4;
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start the HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < count; i++) {
        httpd_register_uri_handler(server, &HANDLERS[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "Settings page is on port %d%s", cfg.server_port, s_https ? " (redirect to HTTPS)" : "");
    // mDNS: the browsers of the home network see the page as a service of <name>.local.
    net_mdns_service("_http", cfg.server_port, s_version);
    if (s_https) {
        net_mdns_service("_https", 443, s_version);
    }
    return ESP_OK;
}
