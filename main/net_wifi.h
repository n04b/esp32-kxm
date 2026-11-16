#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Рабочий режим: STA, Modem Sleep выключен, бесконечные переподключения. */
esp_err_t net_wifi_start_sta(const char *ssid, const char *pass);

/* Первичная настройка: точка доступа kvm-setup-XXXX на 192.168.4.1. */
esp_err_t net_wifi_start_setup_ap(void);

bool net_wifi_connected(void);
bool net_wifi_setup_mode(void);
uint32_t net_wifi_sta_ip(void);     /* host byte order, 0 — нет */
int  net_wifi_rssi(void);
const char *net_wifi_ssid(void);
const char *net_wifi_ap_ssid(void);

/* Блокирующее сканирование; пишет JSON-массив в buf. */
esp_err_t net_wifi_scan_json(char *buf, size_t len);

/* Вызывается при каждом получении IP (после первого — переподключение). */
typedef void (*net_wifi_ip_cb_t)(bool reconnect);
void net_wifi_set_ip_cb(net_wifi_ip_cb_t cb);

/* Ждать подключения (мс, portMAX_DELAY — бесконечно). */
bool net_wifi_wait_connected(uint32_t timeout_ms);
