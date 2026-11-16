#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Normal mode: STA, Modem Sleep off, endless reconnects. */
esp_err_t net_wifi_start_sta(const char *ssid, const char *pass);

/* Initial setup: access point kvm-setup-XXXX at 192.168.4.1. */
esp_err_t net_wifi_start_setup_ap(void);

bool net_wifi_connected(void);
bool net_wifi_setup_mode(void);
uint32_t net_wifi_sta_ip(void);     /* host byte order, 0 = none */
int  net_wifi_rssi(void);
const char *net_wifi_ssid(void);
const char *net_wifi_ap_ssid(void);

/* Blocking scan; writes a JSON array into buf. */
esp_err_t net_wifi_scan_json(char *buf, size_t len);

/* Called on every IP acquisition (after the first one it means a reconnect). */
typedef void (*net_wifi_ip_cb_t)(bool reconnect);
void net_wifi_set_ip_cb(net_wifi_ip_cb_t cb);

/* Wait for connection (ms, portMAX_DELAY = forever). */
bool net_wifi_wait_connected(uint32_t timeout_ms);
