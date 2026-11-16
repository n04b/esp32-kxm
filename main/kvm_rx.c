#include "kvm_rx.h"
#include "kvm_input.h"
#include "app_config.h"
#include "net_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "kvm_rx";

static uint16_t s_port;
static volatile bool s_lan_on;

void kvm_rx_from_tailnet(uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len)
{
    /* Пустой список — ввод не принимается ни от кого. */
    if (!config_ip_allowed(src_ip)) {
        kvm_input_count_denied();
        return;
    }
    kvm_source_t src = { .kind = KVM_SRC_UDP_TS, .ip = src_ip, .port = src_port, .fd = -1 };
    kvm_input_packet(&src, data, len);
}

void kvm_rx_set_lan_enabled(bool on) { s_lan_on = on; }

static int open_lan_socket(uint32_t ip)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    /* Привязка к адресу STA, а не INADDR_ANY: порт на tailnet-адресе
     * уже занят сокетом MicroLink, и lwIP не даёт их совместить. */
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr.s_addr = htonl(ip),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "bind LAN UDP: errno %d", errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "Приём из LAN на порту %u", s_port);
    return fd;
}

static void lan_task(void *arg)
{
    (void)arg;
    int fd = -1;
    uint32_t bound_ip = 0;
    uint8_t buf[64];

    for (;;) {
        uint32_t ip = net_wifi_sta_ip();
        if (!s_lan_on || ip == 0) {
            if (fd >= 0) { close(fd); fd = -1; }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (fd < 0 || ip != bound_ip) {
            if (fd >= 0) close(fd);
            fd = open_lan_socket(ip);
            bound_ip = ip;
            if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        uint32_t src_ip = ntohl(from.sin_addr.s_addr);
        if (!config_ip_allowed(src_ip)) {
            kvm_input_count_denied();
            continue;
        }
        kvm_source_t src = { .kind = KVM_SRC_UDP_LAN, .ip = src_ip,
                             .port = ntohs(from.sin_port), .fd = -1 };
        kvm_input_packet(&src, buf, n);
    }
}

esp_err_t kvm_rx_start(uint16_t port)
{
    s_port = port;
    if (xTaskCreatePinnedToCore(lan_task, "kvm_rx_lan", 3072, NULL,
                                configMAX_PRIORITIES - 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
