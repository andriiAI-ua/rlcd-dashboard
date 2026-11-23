// Wi-Fi: main and backup networks, the setup access point, DNS servers and SNTP.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*net_change_cb_t)(void);

// Start Wi-Fi in a separate task:
// 1. Try the main network, then the backup network, two times.
// 2. If no network connects, start the setup access point (SSID "RLCD-xxxx", address CONFIG_RLCD_AP_IP).
//    Every 5 minutes, when no client uses the access point, try the networks again.
// 3. When the device is connected and the access point has no clients for 10 minutes, stop it.
// on_change runs when the state changes. It runs in a system task: keep it short.
void net_start(const settings_t *settings, const char *ntp_server, net_change_cb_t on_change);

// Start the setup access point now (for example, after a long press of BOOT).
void net_request_ap(void);

// Eco mode: turn Wi-Fi off and on again. While Wi-Fi is off, the device does not try to connect.
void net_suspend(void);
void net_resume(void);
bool net_suspended(void);

// Allow or forbid the automatic start of the access point when no network connects.
// Eco mode forbids it: the device must not stay awake when the router is off.
void net_set_auto_ap(bool on);

bool net_connected(void);

// Return the name of the connected network, or "".
const char *net_ssid(void);

// Return the signal level of the access point, dBm, or 0 when not connected.
int net_rssi(void);

// Write the IP address ("192.168.1.10") or an empty string.
void net_ip(char *buf, size_t len);

// Return the mDNS host name without ".local" ("rlcd"): the first label of the device name. "" if mDNS is off.
const char *net_mdns_host(void);

// Announce a service of the device over mDNS, for example "_http" on port 80. version goes to the TXT record.
void net_mdns_service(const char *type, unsigned port, const char *version);

// Setup access point
bool net_ap_active(void);
const char *net_ap_ssid(void);
const char *net_ap_password(void);
const char *net_ap_ip(void);

// Return the reason code of the last Wi-Fi disconnection (wifi_err_reason_t), or 0.
int net_last_reason(void);

// Return the number of time synchronizations since the start.
unsigned net_sync_count(void);

// Return the time of the last time synchronization, or 0.
time_t net_sync_time(void);

#ifdef __cplusplus
}
#endif
