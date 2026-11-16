/*
 * ESP32-S3 KVM over Tailscale.
 * USB HID (boot keyboard + mouse) <- input from the web UI (WebSocket)
 * or UDP using the esp32-kvm-ip protocol over the tailnet (MicroLink).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "microlink.h"

#include "app_config.h"
#include "kvm_input.h"
#include "kvm_rx.h"
#include "net_wifi.h"
#include "status.h"
#include "ts_client.h"
#include "usb_hid.h"
#include "web_ui.h"

static const char *TAG = "main";

static void mark_app_valid_when_healthy(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    /* New firmware is marked valid once WiFi and Tailscale are up
     * (or after 5 minutes of running without a reboot). */
    int64_t t0 = esp_timer_get_time();
    while (!(net_wifi_connected() && ts_client_connected()) &&
           esp_timer_get_time() - t0 < 300LL * 1000000) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Firmware confirmed (rollback cancelled)");
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 KVM %s", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    config_init();
    if (config_take_rereg_flag()) {
        ESP_LOGW(TAG, "Re-registering with Tailscale: node keys erased");
        microlink_factory_reset();
    }

    kvm_settings_t cfg;
    config_get(&cfg);

    kvm_input_init();
    ESP_ERROR_CHECK(usb_hid_start());

    if (!cfg.provisioned) {
        net_wifi_start_setup_ap();
        status_start();
        ESP_ERROR_CHECK(web_ui_start(true));
        return;
    }

    net_wifi_start_sta(cfg.wifi_ssid, cfg.wifi_pass);
    status_start();
    kvm_rx_start(cfg.udp_port);
    ts_client_start(&cfg);
    ESP_ERROR_CHECK(web_ui_start(false));

    mark_app_valid_when_healthy();
}
