/* Приём UDP-пакетов ввода: из tailnet (сокет MicroLink) и, по настройке, из LAN. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Запускает задачу приёма из локальной сети (ядро 1). */
esp_err_t kvm_rx_start(uint16_t port);

/* Приём LAN включается/выключается на лету. */
void kvm_rx_set_lan_enabled(bool on);

/* Колбэк MicroLink: пакет из tailnet. */
void kvm_rx_from_tailnet(uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len);
