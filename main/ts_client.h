/* MicroLink wrapper: tailnet registration, UDP input socket. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_config.h"

esp_err_t ts_client_start(const kvm_settings_t *cfg);

bool ts_client_connected(void);
uint32_t ts_client_vpn_ip(void);
const char *ts_client_state_str(void);
/* Error message for the UI, or NULL. */
const char *ts_client_error(void);
/* Path to the peer: 1 = direct, 0 = DERP, -1 = unknown peer. */
int ts_client_peer_direct(uint32_t ip);
/* JSON array of peers. */
void ts_client_peers_json(char *buf, size_t len);
