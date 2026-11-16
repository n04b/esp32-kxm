#include "status.h"
#include "app_config.h"
#include "kvm_input.h"
#include "net_wifi.h"
#include "ts_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#if CONFIG_KVM_LED_WS2812
#include "led_strip.h"
#endif

static const char *TAG = "status";

typedef enum { ST_SETUP, ST_NO_WIFI, ST_NO_TS, ST_OK, ST_INPUT } led_state_t;

#if CONFIG_KVM_LED_WS2812
static led_strip_handle_t s_strip;
#endif

static void led_set(bool on, uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_KVM_LED_GPIO >= 0
#if CONFIG_KVM_LED_WS2812
    if (!s_strip) return;
    if (on) led_strip_set_pixel(s_strip, 0, r, g, b);
    else    led_strip_set_pixel(s_strip, 0, 0, 0, 0);
    led_strip_refresh(s_strip);
#else
    (void)r; (void)g; (void)b;
#if CONFIG_KVM_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_KVM_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_KVM_LED_GPIO, on ? 1 : 0);
#endif
#endif
#endif
}

static void led_init(void)
{
#if CONFIG_KVM_LED_GPIO >= 0
#if CONFIG_KVM_LED_WS2812
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_KVM_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 on GPIO%d failed to initialize", CONFIG_KVM_LED_GPIO);
        s_strip = NULL;
    }
#else
    gpio_reset_pin(CONFIG_KVM_LED_GPIO);
    gpio_set_direction(CONFIG_KVM_LED_GPIO, GPIO_MODE_OUTPUT);
#endif
#endif
}

static led_state_t current_state(void)
{
    if (net_wifi_setup_mode()) return ST_SETUP;
    if (!net_wifi_connected()) return ST_NO_WIFI;
    if (!ts_client_connected()) return ST_NO_TS;
    kvm_stats_t st;
    kvm_input_get_stats(&st);
    if (st.last_input_us && esp_timer_get_time() - st.last_input_us < 150000) return ST_INPUT;
    return ST_OK;
}

static void status_task(void *arg)
{
    (void)arg;
    gpio_config_t bc = {
        .pin_bit_mask = 1ULL << CONFIG_KVM_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&bc);
    led_init();

    int64_t pressed_since = 0;
    uint32_t tick = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        tick++;

        /* ── BOOT button ── */
        if (gpio_get_level(CONFIG_KVM_BOOT_BUTTON_GPIO) == 0) {
            int64_t now = esp_timer_get_time();
            if (!pressed_since) pressed_since = now;
            int64_t held = (now - pressed_since) / 1000;
            if (held >= CONFIG_KVM_FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held for %d ms, factory reset", CONFIG_KVM_FACTORY_RESET_HOLD_MS);
                for (int i = 0; i < 10; i++) {
                    led_set(i & 1, 255, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(80));
                }
                config_factory_reset();
                esp_restart();
            }
            /* while held: fast white blinking */
            led_set((tick & 1), 40, 40, 40);
            continue;
        }
        pressed_since = 0;

        /* ── LED ── */
        switch (current_state()) {
        case ST_SETUP:   led_set((tick / 10) & 1, 0, 0, 40); break;        /* blue, 1 Hz */
        case ST_NO_WIFI: led_set((tick / 2) & 1, 40, 0, 0); break;         /* red, fast */
        case ST_NO_TS:   led_set((tick / 10) & 1, 40, 25, 0); break;       /* yellow, slow */
        case ST_OK:      led_set(true, 0, 20, 0); break;                   /* green */
        case ST_INPUT:   led_set((tick & 1), 0, 40, 40); break;            /* flicker */
        }
    }
}

esp_err_t status_start(void)
{
    return xTaskCreatePinnedToCore(status_task, "status", 3072, NULL, 2, NULL, 0) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
