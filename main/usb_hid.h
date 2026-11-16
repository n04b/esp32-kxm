#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Start TinyUSB and the HID report send task (core 1). */
esp_err_t usb_hid_start(void);

/* Wake the HID task: there is new state to send. */
void usb_hid_kick(void);

bool usb_hid_mounted(void);
bool usb_hid_suspended(void);
bool usb_hid_kbd_boot_protocol(void);

/* Keyboard indicators from the host: bit0 Num, bit1 Caps, bit2 Scroll. */
uint8_t usb_hid_leds(void);

/* Called when the indicators change (from TinyUSB context). */
typedef void (*usb_hid_leds_cb_t)(uint8_t leds);
void usb_hid_set_leds_cb(usb_hid_leds_cb_t cb);
