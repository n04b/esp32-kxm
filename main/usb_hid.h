#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Запуск TinyUSB и задачи отправки HID-отчётов (ядро 1). */
esp_err_t usb_hid_start(void);

/* Разбудить задачу HID: есть новое состояние для отправки. */
void usb_hid_kick(void);

bool usb_hid_mounted(void);
bool usb_hid_suspended(void);
bool usb_hid_kbd_boot_protocol(void);

/* Индикаторы клавиатуры от хоста: bit0 Num, bit1 Caps, bit2 Scroll. */
uint8_t usb_hid_leds(void);

/* Вызывается при изменении индикаторов (из контекста TinyUSB). */
typedef void (*usb_hid_leds_cb_t)(uint8_t leds);
void usb_hid_set_leds_cb(usb_hid_leds_cb_t cb);
