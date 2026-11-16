/*
 * USB HID: two interfaces.
 *   ITF 0: keyboard, boot protocol, standard 8-byte report without report ID
 *           (works in BIOS/UEFI), output: Num/Caps/Scroll indicators.
 *   ITF 1: mouse (report ID 1: 5 buttons, 16-bit X/Y, wheel, AC Pan, as in
 *           esp32-kvm-ip) + consumer control (report ID 3). Also supports the
 *           mouse boot protocol (3 bytes), but the mouse is not guaranteed in BIOS.
 * 1 ms polling. The send task runs on core 1.
 */
#include "usb_hid.h"
#include "kvm_input.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb_hid";

#define ITF_KBD   0
#define ITF_MOUSE 1
#define RID_MOUSE    1
#define RID_CONSUMER 3

#define EP_KBD   0x81
#define EP_MOUSE 0x82

static TaskHandle_t s_task;
static volatile uint8_t s_leds;
static usb_hid_leds_cb_t s_leds_cb;

/* ── Report descriptors ──────────────────────────────────────────── */

static const uint8_t s_kbd_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

static const uint8_t s_mouse_report_desc[] = {
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP ),
    HID_USAGE      ( HID_USAGE_DESKTOP_MOUSE ),
    HID_COLLECTION ( HID_COLLECTION_APPLICATION ),
      HID_REPORT_ID( RID_MOUSE )
      HID_USAGE      ( HID_USAGE_DESKTOP_POINTER ),
      HID_COLLECTION ( HID_COLLECTION_PHYSICAL ),
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_BUTTON ),
        HID_USAGE_MIN   ( 1 ),
        HID_USAGE_MAX   ( 5 ),
        HID_LOGICAL_MIN ( 0 ),
        HID_LOGICAL_MAX ( 1 ),
        HID_REPORT_COUNT( 5 ),
        HID_REPORT_SIZE ( 1 ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),
        HID_REPORT_COUNT( 1 ),
        HID_REPORT_SIZE ( 3 ),
        HID_INPUT       ( HID_CONSTANT ),
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP ),
        HID_USAGE       ( HID_USAGE_DESKTOP_X ),
        HID_USAGE       ( HID_USAGE_DESKTOP_Y ),
        HID_LOGICAL_MIN_N( -32767, 2 ),
        HID_LOGICAL_MAX_N(  32767, 2 ),
        HID_REPORT_SIZE ( 16 ),
        HID_REPORT_COUNT( 2 ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ),
        HID_USAGE       ( HID_USAGE_DESKTOP_WHEEL ),
        HID_LOGICAL_MIN ( -127 ),
        HID_LOGICAL_MAX (  127 ),
        HID_REPORT_SIZE ( 8 ),
        HID_REPORT_COUNT( 1 ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ),
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_CONSUMER ),
        HID_USAGE_N     ( HID_USAGE_CONSUMER_AC_PAN, 2 ),
        HID_LOGICAL_MIN ( -127 ),
        HID_LOGICAL_MAX (  127 ),
        HID_REPORT_SIZE ( 8 ),
        HID_REPORT_COUNT( 1 ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ),
      HID_COLLECTION_END,
    HID_COLLECTION_END,
    TUD_HID_REPORT_DESC_CONSUMER( HID_REPORT_ID(RID_CONSUMER) )
};

/* ── Configuration descriptor ────────────────────────────────────── */

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_KBD, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_kbd_report_desc), EP_KBD, 8, 1),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 5, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(s_mouse_report_desc), EP_MOUSE, 8, 1),
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_KVM_USB_VID,
    .idProduct          = CONFIG_KVM_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

static char s_serial[16];
static const char *s_strings[] = {
    (const char[]){ 0x09, 0x04 },
    CONFIG_KVM_USB_MANUFACTURER,
    CONFIG_KVM_USB_PRODUCT,
    s_serial,
    "Keyboard",
    "Mouse",
};

/* ── TinyUSB callbacks ───────────────────────────────────────────── */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == ITF_KBD ? s_kbd_report_desc : s_mouse_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    if (instance != ITF_KBD || report_type != HID_REPORT_TYPE_OUTPUT || bufsize < 1) return;
    uint8_t leds = buffer[bufsize - 1] & 0x07;   /* no report ID: a single byte */
    if (leds != s_leds) {
        s_leds = leds;
        if (s_leds_cb) s_leds_cb(leds);
    }
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    ESP_LOGI(TAG, "HID %s: protocol %s", instance == ITF_KBD ? "keyboard" : "mouse",
             protocol == HID_PROTOCOL_BOOT ? "boot" : "report");
    kvm_input_mark_all_dirty();
    usb_hid_kick();
}

/* ── Send task ───────────────────────────────────────────────────── */

static bool service_reports(void)
{
    bool pending = false;

    /* Keyboard: the report is the same in boot and report modes. */
    if (tud_hid_n_ready(ITF_KBD)) {
        uint8_t rep[8];
        if (kvm_input_take_kbd(rep)) {
            tud_hid_n_report(ITF_KBD, 0, rep, sizeof(rep));
            pending = true;
        }
    } else {
        pending = true;
    }

    if (tud_hid_n_ready(ITF_MOUSE)) {
        bool boot = tud_hid_n_get_protocol(ITF_MOUSE) == HID_PROTOCOL_BOOT;
        uint8_t b; int16_t dx, dy; int8_t w, p;
        uint16_t usage;
        if (kvm_input_take_mouse(boot ? 127 : 32767, &b, &dx, &dy, &w, &p)) {
            if (boot) {
                uint8_t rep[4] = { b & 0x07, (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, (uint8_t)w };
                tud_hid_n_report(ITF_MOUSE, 0, rep, 3);
            } else {
                uint8_t rep[7] = { b, (uint8_t)dx, (uint8_t)(dx >> 8), (uint8_t)dy,
                                   (uint8_t)(dy >> 8), (uint8_t)w, (uint8_t)p };
                tud_hid_n_report(ITF_MOUSE, RID_MOUSE, rep, sizeof(rep));
            }
            pending = true;
        } else if (!boot && kvm_input_take_consumer(&usage)) {
            uint8_t rep[2] = { (uint8_t)usage, (uint8_t)(usage >> 8) };
            tud_hid_n_report(ITF_MOUSE, RID_CONSUMER, rep, sizeof(rep));
            pending = true;
        }
    } else {
        pending = true;
    }
    return pending;
}

static void hid_task(void *arg)
{
    (void)arg;
    bool was_mounted = false;
    bool pending = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pending ? 1 : 5));
        kvm_input_tick();

        bool mounted = tud_mounted();
        if (mounted && !was_mounted) {
            ESP_LOGI(TAG, "USB connected to the target PC");
            kvm_input_mark_all_dirty();
        } else if (!mounted && was_mounted) {
            ESP_LOGW(TAG, "USB disconnected");
        }
        was_mounted = mounted;

        if (!mounted) {
            /* Target PC is off: packets are silently dropped. */
            kvm_input_discard_motion();
            pending = false;
            continue;
        }
        if (tud_suspended()) {
            kvm_input_discard_motion();
            if (kvm_input_wants_wakeup()) {
                tud_remote_wakeup();
            }
            pending = false;
            continue;
        }
        pending = service_reports();
    }
}

void usb_hid_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

bool usb_hid_mounted(void)   { return tud_mounted(); }
bool usb_hid_suspended(void) { return tud_suspended(); }
bool usb_hid_kbd_boot_protocol(void)
{
    return tud_mounted() && tud_hid_n_get_protocol(ITF_KBD) == HID_PROTOCOL_BOOT;
}
uint8_t usb_hid_leds(void) { return s_leds; }
void usb_hid_set_leds_cb(usb_hid_leds_cb_t cb) { s_leds_cb = cb; }

esp_err_t usb_hid_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const tinyusb_config_t cfg = {
        .device_descriptor = &s_device_desc,
        .string_descriptor = s_strings,
        .string_descriptor_count = sizeof(s_strings) / sizeof(s_strings[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = s_config_desc,
        .hs_configuration_descriptor = s_config_desc,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = s_config_desc,
#endif
    };
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    /* Stack in SRAM (xTaskCreate uses internal memory), core 1. */
    if (xTaskCreatePinnedToCore(hid_task, "usb_hid", 4096, NULL,
                                configMAX_PRIORITIES - 3, &s_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB HID started: keyboard (boot) + mouse/consumer");
    return ESP_OK;
}
