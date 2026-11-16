/* Receiving UDP input packets: from the tailnet (MicroLink socket) and, if enabled, from LAN. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Starts the local network receive task (core 1). */
esp_err_t kvm_rx_start(uint16_t port);

/* LAN receive can be toggled at runtime. */
void kvm_rx_set_lan_enabled(bool on);

/* MicroLink callback: packet from the tailnet. */
void kvm_rx_from_tailnet(uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len);
