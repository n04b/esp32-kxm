#include "ts_client.h"
#include "kvm_rx.h"
#include "net_wifi.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "microlink.h"
#include "microlink_internal.h"   /* ctrl_host, key_expired */

static const char *TAG = "tailscale";

static microlink_t *s_ml;
static microlink_udp_socket_t *s_udp;
static TaskHandle_t s_task;
static uint16_t s_port;
static bool s_no_key;
static volatile bool s_started;

/* These strings must live as long as MicroLink runs. */
static char s_auth_key[128];
static char s_hostname[48];
static char s_ctrl_host[64];

static void on_state(microlink_t *ml, microlink_state_t st, void *ud)
{
    (void)ml; (void)ud;
    ESP_LOGI(TAG, "State: %d", st);
    if (s_task) xTaskNotifyGive(s_task);
}

static void on_udp_rx(microlink_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port,
                      const uint8_t *data, size_t len, void *ud)
{
    (void)sock; (void)ud;
    kvm_rx_from_tailnet(src_ip, src_port, data, len);
}

static void on_wifi_ip(bool reconnect)
{
    if (reconnect && s_started && s_ml) {
        ESP_LOGI(TAG, "WiFi is back, rebinding MicroLink sockets");
        microlink_rebind(s_ml);
    }
}

static void ts_task(void *arg)
{
    (void)arg;
    net_wifi_wait_connected(portMAX_DELAY);

    if (microlink_start(s_ml) != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed");
        vTaskDelete(NULL);
    }
    s_started = true;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        bool conn = microlink_is_connected(s_ml);
        if (conn && !s_udp) {
            s_udp = microlink_udp_create(s_ml, s_port);
            if (s_udp) {
                microlink_udp_set_rx_callback(s_udp, on_udp_rx, NULL);
                char ip[16];
                microlink_ip_to_str(microlink_get_vpn_ip(s_ml), ip);
                ESP_LOGI(TAG, "Receiving input: %s:%u (UDP over tailnet)", ip, s_port);
            }
        }
    }
}

esp_err_t ts_client_start(const kvm_settings_t *cfg)
{
    net_wifi_set_ip_cb(on_wifi_ip);
    s_port = cfg->udp_port;
    if (!cfg->ts_auth_key[0]) {
        s_no_key = true;
        ESP_LOGW(TAG, "Tailscale auth key not set, tailnet disabled");
        return ESP_OK;
    }
    strlcpy(s_auth_key, cfg->ts_auth_key, sizeof(s_auth_key));
    strlcpy(s_hostname, cfg->ts_hostname[0] ? cfg->ts_hostname : CONFIG_KVM_DEFAULT_HOSTNAME,
            sizeof(s_hostname));
    strlcpy(s_ctrl_host, cfg->ts_ctrl_host, sizeof(s_ctrl_host));

    microlink_config_t mc = {
        .auth_key = s_auth_key,
        .device_name = s_hostname,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_KVM_TS_MAX_PEERS,
        .priority_peer_ip = config_first_allowed(),
    };
    s_ml = microlink_init(&mc);
    if (!s_ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        return ESP_FAIL;
    }
    if (s_ctrl_host[0]) {
        strlcpy(s_ml->ctrl_host, s_ctrl_host, sizeof(s_ml->ctrl_host));
        ESP_LOGI(TAG, "Control server: %s", s_ctrl_host);
    }
    microlink_set_state_callback(s_ml, on_state, NULL);

    if (xTaskCreatePinnedToCore(ts_task, "ts_client", 4096, NULL, 4, &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ts_client_connected(void) { return s_ml && microlink_is_connected(s_ml); }
uint32_t ts_client_vpn_ip(void) { return s_ml ? microlink_get_vpn_ip(s_ml) : 0; }

const char *ts_client_state_str(void)
{
    if (s_no_key) return "no_key";
    if (!s_ml) return "off";
    switch (microlink_get_state(s_ml)) {
    case ML_STATE_IDLE:         return "idle";
    case ML_STATE_WIFI_WAIT:    return "wifi_wait";
    case ML_STATE_CONNECTING:   return "connecting";
    case ML_STATE_REGISTERING:  return "registering";
    case ML_STATE_CONNECTED:    return "connected";
    case ML_STATE_RECONNECTING: return "reconnecting";
    case ML_STATE_ERROR:        return "error";
    }
    return "unknown";
}

const char *ts_client_error(void)
{
    if (s_no_key) return "Tailscale auth key is not set";
    if (!s_ml) return NULL;
    if (s_ml->key_expired)
        return "Node key expired. If the auth key is also invalid, enter a new one and press \"Re-register\"";
    if (microlink_get_state(s_ml) == ML_STATE_ERROR)
        return "Tailnet registration failed: check the auth key and control server address";
    return NULL;
}

int ts_client_peer_direct(uint32_t ip)
{
    if (!s_ml) return -1;
    int n = microlink_get_peer_count(s_ml);
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t p;
        if (microlink_get_peer_info(s_ml, i, &p) == ESP_OK && p.vpn_ip == ip) {
            return p.direct_path ? 1 : 0;
        }
    }
    return -1;
}

void ts_client_peers_json(char *buf, size_t len)
{
    size_t p = snprintf(buf, len, "[");
    int n = s_ml ? microlink_get_peer_count(s_ml) : 0;
    for (int i = 0; i < n && p + 160 < len; i++) {
        microlink_peer_info_t info;
        if (microlink_get_peer_info(s_ml, i, &info) != ESP_OK) continue;
        char ip[16], host[64];
        microlink_ip_to_str(info.vpn_ip, ip);
        /* hostname from the tailnet: strip quotes and backslashes */
        size_t j = 0;
        for (size_t k = 0; info.hostname[k] && j < sizeof(host) - 1; k++) {
            char c = info.hostname[k];
            if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) host[j++] = c;
        }
        host[j] = 0;
        p += snprintf(buf + p, len - p,
                      "%s{\"ip\":\"%s\",\"host\":\"%s\",\"online\":%s,\"direct\":%s,\"allowed\":%s}",
                      p > 1 ? "," : "", ip, host, info.online ? "true" : "false",
                      info.direct_path ? "true" : "false",
                      config_ip_allowed(info.vpn_ip) ? "true" : "false");
    }
    snprintf(buf + p, len - p, "]");
}
