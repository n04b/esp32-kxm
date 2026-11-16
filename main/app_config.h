/* Device settings in NVS (namespace "kvm"). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define KVM_MAX_ALLOWED 16
#define KVM_CFG_VERSION 1

typedef struct {
    uint32_t version;
    bool     provisioned;          /* initial setup complete */

    char     wifi_ssid[33];
    char     wifi_pass[65];

    char     ts_auth_key[128];
    char     ts_ctrl_host[64];     /* empty: official Tailscale */
    char     ts_hostname[48];

    uint8_t  pw_salt[16];
    uint8_t  pw_hash[32];          /* PBKDF2-HMAC-SHA256 */
    bool     pw_set;

    uint16_t udp_port;
    uint16_t wd_ms;                /* watchdog for WebSocket */
    uint16_t udp_wd_ms;            /* watchdog for UDP, 0 = off */
    bool     lan_rx;               /* receive UDP from the local network */

    bool     web_lan;              /* web UI on the local address */
    bool     web_ts;               /* web UI on the tailnet address */

    uint8_t  allow_count;
    uint32_t allow_ip[KVM_MAX_ALLOWED];   /* host byte order */
    char     allow_label[KVM_MAX_ALLOWED][24];
} kvm_settings_t;

esp_err_t config_init(void);

/* Copy of the current settings. */
void config_get(kvm_settings_t *out);

/* Write the whole settings struct (to RAM and NVS). */
esp_err_t config_set(const kvm_settings_t *in);

/* Fast allowlist check (callable from any task context). */
bool config_ip_allowed(uint32_t ip);
uint32_t config_first_allowed(void);

/* Web UI password. */
esp_err_t config_hash_password(const char *password, kvm_settings_t *cfg);
bool config_check_password(const char *password);

/* "Re-register with Tailscale on next boot" flag. */
void config_set_rereg_flag(bool on);
bool config_take_rereg_flag(void);

/* Reset: erases KVM settings and MicroLink keys. Does not reboot. */
esp_err_t config_factory_reset(void);
