/* Настройки устройства в NVS (namespace "kvm"). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define KVM_MAX_ALLOWED 16
#define KVM_CFG_VERSION 1

typedef struct {
    uint32_t version;
    bool     provisioned;          /* первичная настройка завершена */

    char     wifi_ssid[33];
    char     wifi_pass[65];

    char     ts_auth_key[128];
    char     ts_ctrl_host[64];     /* пусто — официальный Tailscale */
    char     ts_hostname[48];

    uint8_t  pw_salt[16];
    uint8_t  pw_hash[32];          /* PBKDF2-HMAC-SHA256 */
    bool     pw_set;

    uint16_t udp_port;
    uint16_t wd_ms;                /* сторож для WebSocket */
    uint16_t udp_wd_ms;            /* сторож для UDP, 0 — выключен */
    bool     lan_rx;               /* приём UDP из локальной сети */

    bool     web_lan;              /* веб-интерфейс по локальному адресу */
    bool     web_ts;               /* веб-интерфейс по tailnet-адресу */

    uint8_t  allow_count;
    uint32_t allow_ip[KVM_MAX_ALLOWED];   /* host byte order */
    char     allow_label[KVM_MAX_ALLOWED][24];
} kvm_settings_t;

esp_err_t config_init(void);

/* Копия текущих настроек. */
void config_get(kvm_settings_t *out);

/* Записать настройки целиком (в RAM и NVS). */
esp_err_t config_set(const kvm_settings_t *in);

/* Быстрая проверка списка разрешённых (можно из любого контекста задачи). */
bool config_ip_allowed(uint32_t ip);
uint32_t config_first_allowed(void);

/* Пароль веб-интерфейса. */
esp_err_t config_hash_password(const char *password, kvm_settings_t *cfg);
bool config_check_password(const char *password);

/* Флаг «перерегистрироваться в Tailscale при следующем старте». */
void config_set_rereg_flag(bool on);
bool config_take_rereg_flag(void);

/* Сброс: стирает настройки KVM и ключи MicroLink. Не перезагружает. */
esp_err_t config_factory_reset(void);
