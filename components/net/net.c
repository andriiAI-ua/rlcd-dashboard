// Wi-Fi with a main and a backup network, a setup access point with a captive portal, DNS servers and SNTP.
// A separate task does the connection steps. The event handler only records the events.
#include "net.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
// dns_server.h uses esp_ip4_addr_t, but does not include esp_netif.h itself.
#include "dns_server.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define CONNECT_TIMEOUT_MS  20000            // for one network
#define AP_RETRY_MS         (5 * 60 * 1000)  // try the networks again while the access point is on
#define AP_IDLE_MS          (10 * 60 * 1000) // stop an unused access point when the device is connected
#define CHECK_PERIOD_MS     60000

#define BIT_GOT_IP      BIT0
#define BIT_DISCONNECT  BIT1
#define BIT_AP_REQUEST  BIT2
#define BIT_RESUME      BIT3

typedef struct {
    char ssid[33];
    char pass[65];
} network_t;

static network_t s_networks[2];
static int s_network_count;
static char s_dns[2][16];
static char s_ap_ssid[33];
static char s_ap_pass[16];
static char s_ap_ip[16];
static char s_portal_uri[32];  // the DHCP server keeps a pointer to it

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static EventGroupHandle_t s_events;
static net_change_cb_t s_on_change;
static dns_server_handle_t s_dns_server;

static volatile bool s_connected;
static volatile bool s_ap_on;
static volatile int s_ap_clients;
static volatile int64_t s_ap_idle_since;
static volatile int s_last_reason;
static volatile unsigned s_sync_count;
static bool s_sntp_started;
static char s_ssid[33];
static volatile bool s_suspended;   // Wi-Fi is off (eco mode)
static volatile bool s_auto_ap = true;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void notify(void)
{
    if (s_on_change) {
        s_on_change();
    }
}

// Use the DNS servers from the settings instead of the DHCP ones.
// DHCP can set its servers again when it renews the lease, so the task calls this every minute.
static void apply_dns(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_dns[i][0] == '\0') {
            continue;
        }
        esp_netif_dns_info_t dns = { 0 };
        if (esp_netif_str_to_ip4(s_dns[i], &dns.ip.u_addr.ip4) != ESP_OK) {
            ESP_LOGW(TAG, "DNS %d \"%s\" is not an IPv4 address", i + 1, s_dns[i]);
            continue;
        }
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta, i == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &dns);
    }
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_sync_count++;
    ESP_LOGI(TAG, "Time is synchronized");
    notify();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        s_last_reason = e->reason;
        const bool was_connected = s_connected;
        s_connected = false;
        xEventGroupSetBits(s_events, BIT_DISCONNECT);
        if (was_connected) {
            ESP_LOGW(TAG, "Disconnected from \"%s\", reason %d", s_ssid, e->reason);
            notify();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
        if (s_ap_clients == 0) {
            s_ap_idle_since = now_ms();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Connected to \"%s\", IP address " IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_last_reason = 0;
        apply_dns();
        if (!s_sntp_started) {
            s_sntp_started = esp_netif_sntp_start() == ESP_OK;
        }
        xEventGroupSetBits(s_events, BIT_GOT_IP);
        notify();
    }
}

static void disconnect_sta(void)
{
    xEventGroupClearBits(s_events, BIT_DISCONNECT);
    if (esp_wifi_disconnect() == ESP_OK) {
        // Wait for the event, so that it does not stop the next attempt.
        xEventGroupWaitBits(s_events, BIT_DISCONNECT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    }
}

// Try to connect to one network. Return true when the device has an IP address.
static bool try_network(const network_t *n, int timeout_ms)
{
    ESP_LOGI(TAG, "Connect to \"%s\"", n->ssid);
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, n->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, n->pass, sizeof(cfg.sta.password));
    // Find the strongest access point of the network (for example, with repeaters).
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return false;
    }
    strlcpy(s_ssid, n->ssid, sizeof(s_ssid));

    const int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECT);
        esp_wifi_connect();
        const int64_t left = deadline - now_ms();
        if (left <= 0) {
            break;
        }
        const EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DISCONNECT | BIT_AP_REQUEST,
                                                     pdFALSE, pdFALSE, pdMS_TO_TICKS(left));
        if (bits & BIT_GOT_IP) {
            return true;
        }
        if ((bits & BIT_AP_REQUEST) || !(bits & BIT_DISCONNECT) || s_suspended) {
            break;  // the access point is necessary now, Wi-Fi goes off, or the time is over
        }
        // The attempt failed (no network, wrong password). Wait a little and try again.
        if (deadline - now_ms() < 3000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s_suspended) {
        return false;  // esp_wifi_stop() ends the attempt
    }
    ESP_LOGW(TAG, "Cannot connect to \"%s\", reason %d", n->ssid, s_last_reason);
    disconnect_sta();
    return false;
}

static bool connect_any(int rounds)
{
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < s_network_count; i++) {
            if ((xEventGroupGetBits(s_events) & BIT_AP_REQUEST) || s_suspended) {
                return false;
            }
            if (try_network(&s_networks[i], CONNECT_TIMEOUT_MS)) {
                return true;
            }
        }
    }
    return false;
}

static void set_ap_address(void)
{
    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_str_to_ip4(s_ap_ip, &ip.ip) != ESP_OK) {
        ESP_LOGE(TAG, "\"%s\" is not an IPv4 address. Use the default address.", s_ap_ip);
        esp_netif_get_ip_info(s_ap, &ip);
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        ip.gw = ip.ip;
        esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(s_ap);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_ip_info(s_ap, &ip));
    }
    // DHCP option 114 tells the phone the address of the captive portal.
    esp_netif_dhcps_stop(s_ap);
    snprintf(s_portal_uri, sizeof(s_portal_uri), "http://%s", s_ap_ip);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_uri, strlen(s_portal_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ap));
}

static void start_ap(void)
{
    s_ap_idle_since = now_ms();
    if (s_ap_on) {
        return;
    }
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, s_ap_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)cfg.ap.password, s_ap_pass, sizeof(cfg.ap.password));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = 4;
    cfg.ap.channel = 1;  // when the station is connected, the access point uses its channel
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    set_ap_address();
    if (!s_dns_server) {
        // Answer all DNS names with the device address, so that phones open the settings page.
        // The DNS server stays on: its stop function does not close the socket.
        dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns_server = start_dns_server(&dns);
    }
    s_ap_clients = 0;
    s_ap_on = true;
    ESP_LOGW(TAG, "Setup access point \"%s\", password %s, http://%s", s_ap_ssid, s_ap_pass, s_ap_ip);
    notify();
}

static void stop_ap(void)
{
    if (!s_ap_on) {
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_on = false;
    ESP_LOGI(TAG, "The setup access point is off");
    notify();
}

static void net_task(void *arg)
{
    (void)arg;
    int64_t last_try = -AP_RETRY_MS;
    for (;;) {
        if (s_suspended) {
            // Wi-Fi is off (eco mode). Wait for net_resume().
            xEventGroupWaitBits(s_events, BIT_RESUME, pdTRUE, pdFALSE, portMAX_DELAY);
            last_try = -AP_RETRY_MS;
            continue;
        }
        if (xEventGroupClearBits(s_events, BIT_AP_REQUEST) & BIT_AP_REQUEST) {
            start_ap();
        }
        // In eco mode the access point does not start by itself: without Wi-Fi (for example,
        // the router has no power) the device sleeps and tries again at the next update.
        const bool auto_ap = s_auto_ap && !s_suspended;
        if (!s_connected) {
            const bool may_try = s_network_count > 0 &&
                (!s_ap_on || (s_ap_clients == 0 && now_ms() - last_try >= AP_RETRY_MS));
            if (may_try) {
                last_try = now_ms();
                if (!connect_any(s_ap_on ? 1 : 2) && !s_ap_on && auto_ap && !s_suspended) {
                    start_ap();
                }
                continue;
            }
            if (!s_ap_on && auto_ap) {
                start_ap();  // no network in the settings
            }
        } else if (s_ap_on && s_ap_clients == 0 && now_ms() - s_ap_idle_since >= AP_IDLE_MS) {
            stop_ap();
        }

        xEventGroupWaitBits(s_events, BIT_DISCONNECT | BIT_AP_REQUEST, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(CHECK_PERIOD_MS));
        xEventGroupClearBits(s_events, BIT_DISCONNECT);
        if (s_connected) {
            apply_dns();
        }
    }
}

void net_start(const settings_t *settings, const char *ntp_server, net_change_cb_t on_change)
{
    s_on_change = on_change;
    s_network_count = 0;
    if (settings->wifi_ssid[0]) {
        strlcpy(s_networks[s_network_count].ssid, settings->wifi_ssid, sizeof(s_networks[0].ssid));
        strlcpy(s_networks[s_network_count].pass, settings->wifi_pass, sizeof(s_networks[0].pass));
        s_network_count++;
    }
    if (settings->wifi2_ssid[0]) {
        strlcpy(s_networks[s_network_count].ssid, settings->wifi2_ssid, sizeof(s_networks[0].ssid));
        strlcpy(s_networks[s_network_count].pass, settings->wifi2_pass, sizeof(s_networks[0].pass));
        s_network_count++;
    }
    strlcpy(s_dns[0], settings->dns1, sizeof(s_dns[0]));
    strlcpy(s_dns[1], settings->dns2, sizeof(s_dns[1]));
    strlcpy(s_ap_pass, settings->ap_pass, sizeof(s_ap_pass));
    strlcpy(s_ap_ip, CONFIG_RLCD_AP_IP, sizeof(s_ap_ip));

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "RLCD-%02X%02X", mac[4], mac[5]);

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();

    // The DHCP host name is the first label of the device name ("rlcd" of "rlcd.lan").
    // Routers with a local DNS (for example, OpenWrt) then resolve "rlcd.lan".
    char label[33];
    size_t n = 0;
    for (const char *p = settings->host; *p && *p != '.' && n + 1 < sizeof(label); p++) {
        label[n++] = *p;
    }
    label[n] = '\0';
    if (label[0]) {
        esp_netif_set_hostname(s_sta, label);
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    // The settings component keeps the networks. The Wi-Fi driver must not keep its own copy.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    // SNTP starts after the first IP address (see on_event).
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
    sntp.start = false;
    sntp.sync_cb = on_time_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(net_task, "net", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "%d Wi-Fi network(s) in the settings", s_network_count);
}

void net_request_ap(void)
{
    if (s_events) {
        xEventGroupSetBits(s_events, BIT_AP_REQUEST);
    }
}

void net_suspend(void)
{
    if (!s_events || s_suspended || s_network_count == 0) {
        return;
    }
    s_suspended = true;
    esp_wifi_stop();
    s_connected = false;
    // Wake the task: it stops its attempt and waits for net_resume().
    xEventGroupSetBits(s_events, BIT_DISCONNECT);
    ESP_LOGI(TAG, "Wi-Fi is off");
}

void net_resume(void)
{
    if (!s_events || !s_suspended) {
        return;
    }
    esp_wifi_start();
    s_suspended = false;
    xEventGroupSetBits(s_events, BIT_RESUME);
    ESP_LOGI(TAG, "Wi-Fi is on");
}

bool net_suspended(void)
{
    return s_suspended;
}

void net_set_auto_ap(bool on)
{
    s_auto_ap = on;
}

bool net_connected(void)
{
    return s_connected;
}

const char *net_ssid(void)
{
    return s_connected ? s_ssid : "";
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

void net_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t info;
    if (s_connected && s_sta && esp_netif_get_ip_info(s_sta, &info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    } else if (len > 0) {
        buf[0] = '\0';
    }
}

bool net_ap_active(void)
{
    return s_ap_on;
}

const char *net_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *net_ap_password(void)
{
    return s_ap_pass;
}

const char *net_ap_ip(void)
{
    return s_ap_ip;
}

int net_last_reason(void)
{
    return s_last_reason;
}

unsigned net_sync_count(void)
{
    return s_sync_count;
}
