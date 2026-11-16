#include "net_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";
#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_ms = 500;
static bool s_setup;
static bool s_had_ip;
static uint32_t s_ip;
static char s_ssid[33];
static char s_ap_ssid[33];
static net_wifi_ip_cb_t s_ip_cb;

static void retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000);
    ESP_LOGI(TAG, "Повтор подключения через %lu мс", (unsigned long)s_retry_ms);
    s_retry_ms *= 2;
    if (s_retry_ms > CONFIG_KVM_WIFI_RETRY_MAX_MS) s_retry_ms = CONFIG_KVM_WIFI_RETRY_MAX_MS;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && !s_setup) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && !s_setup) {
        wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        s_ip = 0;
        ESP_LOGW(TAG, "WiFi отключён, причина %d", d->reason);
        schedule_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_ip = lwip_ntohl(e->ip_info.ip.addr);
        s_retry_ms = 500;
        bool reconnect = s_had_ip;
        s_had_ip = true;
        ESP_LOGI(TAG, "WiFi подключён, IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
        if (s_ip_cb) s_ip_cb(reconnect);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "К точке настройки подключился клиент");
    }
}

static esp_err_t common_init(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry_timer));
    return ESP_OK;
}

esp_err_t net_wifi_start_sta(const char *ssid, const char *pass)
{
    common_init();
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, CONFIG_KVM_DEFAULT_HOSTNAME);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem Sleep выключен: иначе растёт задержка (как в esp32-kvm-ip). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "Подключение к \"%s\"...", ssid);
    return ESP_OK;
}

esp_err_t net_wifi_start_setup_ap(void)
{
    s_setup = true;
    common_init();
    esp_netif_create_default_wifi_ap();
    s_sta = esp_netif_create_default_wifi_sta();   /* нужен для сканирования */

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "kvm-setup-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, CONFIG_KVM_SETUP_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = strlen(CONFIG_KVM_SETUP_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW(TAG, "══════════════════════════════════════════════");
    ESP_LOGW(TAG, " Режим первичной настройки");
    ESP_LOGW(TAG, " Сеть:   %s", s_ap_ssid);
    ESP_LOGW(TAG, " Пароль: %s", CONFIG_KVM_SETUP_AP_PASSWORD);
    ESP_LOGW(TAG, " Адрес:  http://192.168.4.1/");
    ESP_LOGW(TAG, "══════════════════════════════════════════════");
    return ESP_OK;
}

bool net_wifi_connected(void) { return s_ev && (xEventGroupGetBits(s_ev) & BIT_CONNECTED); }
bool net_wifi_setup_mode(void) { return s_setup; }
uint32_t net_wifi_sta_ip(void) { return s_ip; }
const char *net_wifi_ssid(void) { return s_ssid; }
const char *net_wifi_ap_ssid(void) { return s_ap_ssid; }
void net_wifi_set_ip_cb(net_wifi_ip_cb_t cb) { s_ip_cb = cb; }

int net_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!net_wifi_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

bool net_wifi_wait_connected(uint32_t timeout_ms)
{
    TickType_t t = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdFALSE, t) & BIT_CONNECTED;
}

static void json_escape(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 7 < n; i++) {
        unsigned char c = src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c < 0x20) j += snprintf(dst + j, n - j, "\\u%04x", c);
        else dst[j++] = c;
    }
    dst[j] = 0;
}

esp_err_t net_wifi_scan_json(char *buf, size_t len)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) return err;
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) return ESP_ERR_NO_MEM;
    esp_wifi_scan_get_ap_records(&n, recs);
    size_t p = snprintf(buf, len, "[");
    for (int i = 0; i < n && p + 100 < len; i++) {
        char esc[72];
        json_escape(esc, sizeof(esc), (const char *)recs[i].ssid);
        if (!esc[0]) continue;
        p += snprintf(buf + p, len - p, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                      p > 1 ? "," : "", esc, recs[i].rssi,
                      recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    snprintf(buf + p, len - p, "]");
    free(recs);
    return ESP_OK;
}
