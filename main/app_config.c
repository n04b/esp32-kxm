#include "app_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "microlink.h"

static const char *TAG = "config";
#define NVS_NS   "kvm"
#define NVS_KEY  "cfg"
#define PBKDF2_ITER 10000

static kvm_settings_t s_cfg;
static SemaphoreHandle_t s_mtx;

/* Copy of the allowlist for fast access from the receive task. */
static portMUX_TYPE s_allow_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_allow[KVM_MAX_ALLOWED];
static uint8_t  s_allow_n;

static void set_defaults(kvm_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = KVM_CFG_VERSION;
    strlcpy(c->ts_hostname, CONFIG_KVM_DEFAULT_HOSTNAME, sizeof(c->ts_hostname));
    c->udp_port = CONFIG_KVM_UDP_PORT;
    c->wd_ms = CONFIG_KVM_WATCHDOG_MS;
    c->udp_wd_ms = CONFIG_KVM_WATCHDOG_MS;
    c->lan_rx = false;
    c->web_lan = true;
    c->web_ts = true;
}

static void publish_allowlist(const kvm_settings_t *c)
{
    portENTER_CRITICAL(&s_allow_lock);
    s_allow_n = c->allow_count > KVM_MAX_ALLOWED ? KVM_MAX_ALLOWED : c->allow_count;
    memcpy(s_allow, c->allow_ip, sizeof(s_allow));
    portEXIT_CRITICAL(&s_allow_lock);
}

esp_err_t config_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    set_defaults(&s_cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        kvm_settings_t tmp;
        size_t len = sizeof(tmp);
        esp_err_t err = nvs_get_blob(h, NVS_KEY, &tmp, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(tmp) && tmp.version == KVM_CFG_VERSION) {
            s_cfg = tmp;
        } else if (err == ESP_OK) {
            ESP_LOGW(TAG, "Settings of a different version/size, using defaults");
        }
    }
    /* Both web UI addresses cannot be disabled at once. */
    if (!s_cfg.web_lan && !s_cfg.web_ts) s_cfg.web_lan = s_cfg.web_ts = true;
    publish_allowlist(&s_cfg);
    ESP_LOGI(TAG, "Settings: %s, WiFi \"%s\", allowed senders: %u",
             s_cfg.provisioned ? "present" : "none (initial setup)",
             s_cfg.wifi_ssid, s_cfg.allow_count);
    return ESP_OK;
}

void config_get(kvm_settings_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mtx);
}

esp_err_t config_set(const kvm_settings_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_cfg = *in;
    s_cfg.version = KVM_CFG_VERSION;
    if (!s_cfg.web_lan && !s_cfg.web_ts) s_cfg.web_lan = s_cfg.web_ts = true;
    err = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    publish_allowlist(&s_cfg);
    xSemaphoreGive(s_mtx);
    nvs_close(h);
    return err;
}

bool config_ip_allowed(uint32_t ip)
{
    bool ok = false;
    portENTER_CRITICAL(&s_allow_lock);
    for (int i = 0; i < s_allow_n; i++) {
        if (s_allow[i] == ip) { ok = true; break; }
    }
    portEXIT_CRITICAL(&s_allow_lock);
    return ok;
}

uint32_t config_first_allowed(void)
{
    uint32_t ip = 0;
    portENTER_CRITICAL(&s_allow_lock);
    for (int i = 0; i < s_allow_n; i++) {
        /* priority peer: the first tailnet address (100.64.0.0/10) */
        if ((s_allow[i] & 0xFFC00000u) == 0x64400000u) { ip = s_allow[i]; break; }
    }
    portEXIT_CRITICAL(&s_allow_lock);
    return ip;
}

static void pbkdf2(const char *pw, const uint8_t salt[16], uint8_t out[32])
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw),
                                  salt, 16, PBKDF2_ITER, 32, out);
}

esp_err_t config_hash_password(const char *password, kvm_settings_t *cfg)
{
    if (!password || strlen(password) < 6) return ESP_ERR_INVALID_ARG;
    esp_fill_random(cfg->pw_salt, sizeof(cfg->pw_salt));
    pbkdf2(password, cfg->pw_salt, cfg->pw_hash);
    cfg->pw_set = true;
    return ESP_OK;
}

bool config_check_password(const char *password)
{
    kvm_settings_t c;
    config_get(&c);
    if (!c.pw_set || !password) return false;
    uint8_t h[32];
    pbkdf2(password, c.pw_salt, h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= h[i] ^ c.pw_hash[i];
    return diff == 0;
}

void config_set_rereg_flag(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "rereg", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool config_take_rereg_flag(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_get_u8(h, "rereg", &v);
    if (v) {
        nvs_set_u8(h, "rereg", 0);
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    microlink_factory_reset();
    ESP_LOGW(TAG, "Settings erased");
    return ESP_OK;
}
