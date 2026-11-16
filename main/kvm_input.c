#include "kvm_input.h"
#include "sdkconfig.h"
#include "protocol.h"
#include "usb_hid.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "kvm_in";

/* A source is considered free if no packets arrived from it for this many ms,
 * even if its watchdog is disabled (udp_wd_ms = 0). */
#define SOURCE_IDLE_RELEASE_MS 2000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    /* state that should end up on the host */
    uint8_t  kbd[8];
    bool     kbd_dirty;
    uint8_t  buttons;
    int32_t  dx, dy, wheel, pan;
    bool     mouse_dirty;
    uint16_t consumer;
    bool     consumer_dirty;

    /* source */
    kvm_source_t active;
    int64_t  last_rx_us;
    uint32_t last_seq;

    /* watchdog */
    uint32_t wd_ws_ms;
    uint32_t wd_udp_ms;

    /* counters */
    kvm_stats_t st;
    uint32_t rate_count;
    int64_t  rate_window_us;
} s;

bool kvm_source_same(const kvm_source_t *a, const kvm_source_t *b)
{
    if (a->kind != b->kind) return false;
    if (a->kind == KVM_SRC_WS) return a->fd == b->fd;
    return a->ip == b->ip && a->port == b->port;
}

static bool anything_pressed_locked(void)
{
    if (s.kbd[0]) return true;
    for (int i = 2; i < 8; i++) if (s.kbd[i]) return true;
    return s.buttons || s.consumer;
}

static void release_all_locked(void)
{
    memset(s.kbd, 0, sizeof(s.kbd));
    s.kbd_dirty = true;
    s.buttons = 0;
    s.dx = s.dy = s.wheel = s.pan = 0;
    s.mouse_dirty = true;
    s.consumer = 0;
    s.consumer_dirty = true;
    s.active.kind = KVM_SRC_NONE;
}

void kvm_input_init(void)
{
    memset(&s, 0, sizeof(s));
    s.active.fd = -1;
    s.wd_ws_ms = CONFIG_KVM_WATCHDOG_MS;
    s.wd_udp_ms = CONFIG_KVM_WATCHDOG_MS;
    s.rate_window_us = esp_timer_get_time();
}

void kvm_input_set_watchdog(uint32_t ws_ms, uint32_t udp_ms)
{
    portENTER_CRITICAL(&s_lock);
    s.wd_ws_ms = ws_ms;
    s.wd_udp_ms = udp_ms;
    portEXIT_CRITICAL(&s_lock);
}

static inline void count_drop_locked(uint32_t *which)
{
    (*which)++;
    s.st.dropped++;
}

kvm_in_result_t kvm_input_packet(const kvm_source_t *src, const uint8_t *data, size_t len)
{
    if (len != KVM_PACKET_SIZE) {
        portENTER_CRITICAL(&s_lock);
        count_drop_locked(&s.st.dropped_bad);
        portEXIT_CRITICAL(&s_lock);
        return KVM_IN_BAD;
    }
    kvm_packet_t pkt;
    memcpy(&pkt, data, sizeof(pkt));
    if (pkt.magic != KVM_PACKET_MAGIC ||
        pkt.type < KVM_EVT_MOUSE || pkt.type > KVM_EVT_CONSUMER) {
        portENTER_CRITICAL(&s_lock);
        count_drop_locked(&s.st.dropped_bad);
        portEXIT_CRITICAL(&s_lock);
        return KVM_IN_BAD;
    }

    int64_t now = esp_timer_get_time();
    kvm_in_result_t res = KVM_IN_OK;

    portENTER_CRITICAL(&s_lock);
    if (s.active.kind != KVM_SRC_NONE && !kvm_source_same(&s.active, src)) {
        count_drop_locked(&s.st.dropped_busy);
        res = KVM_IN_BUSY;
        goto out;
    }
    if (s.active.kind == KVM_SRC_NONE) {
        s.active = *src;
        s.last_seq = pkt.sequence - 1;   /* accept the first packet of a new source */
    } else {
        /* Ordering filter, as in esp32-kvm-ip (safe against 32-bit wraparound). */
        int32_t diff = (int32_t)(pkt.sequence - s.last_seq);
        if (diff <= 0 && diff > -1000) {
            count_drop_locked(&s.st.dropped_stale);
            res = KVM_IN_STALE;
            goto out;
        }
    }
    s.last_seq = pkt.sequence;
    s.last_rx_us = now;
    s.st.last_input_us = now;
    s.st.rx_total++;
    s.rate_count++;

    switch (pkt.type) {
    case KVM_EVT_MOUSE:
        if (pkt.mouse.buttons != s.buttons || pkt.mouse.dx || pkt.mouse.dy ||
            pkt.mouse.wheel || pkt.mouse.pan) {
            s.buttons = pkt.mouse.buttons & 0x1F;
            s.dx += pkt.mouse.dx;
            s.dy += pkt.mouse.dy;
            s.wheel += pkt.mouse.wheel;
            s.pan += pkt.mouse.pan;
            s.mouse_dirty = true;
        }
        break;
    case KVM_EVT_KEYBOARD: {
        uint8_t kbd[8] = { pkt.keyboard.modifiers, 0 };
        memcpy(&kbd[2], pkt.keyboard.keycodes, 6);
        if (memcmp(kbd, s.kbd, 8) != 0) {
            memcpy(s.kbd, kbd, 8);
            s.kbd_dirty = true;
        }
        break;
    }
    case KVM_EVT_CONSUMER:
        if (pkt.consumer.usage_id != s.consumer) {
            s.consumer = pkt.consumer.usage_id;
            s.consumer_dirty = true;
        }
        break;
    }
out:
    portEXIT_CRITICAL(&s_lock);
    if (res == KVM_IN_OK) usb_hid_kick();
    return res;
}

void kvm_input_count_denied(void)
{
    portENTER_CRITICAL(&s_lock);
    count_drop_locked(&s.st.dropped_denied);
    portEXIT_CRITICAL(&s_lock);
}

void kvm_input_release_source(const kvm_source_t *src)
{
    bool kicked = false;
    portENTER_CRITICAL(&s_lock);
    if (s.active.kind != KVM_SRC_NONE && kvm_source_same(&s.active, src)) {
        release_all_locked();
        kicked = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (kicked) usb_hid_kick();
}

void kvm_input_release_all(void)
{
    portENTER_CRITICAL(&s_lock);
    release_all_locked();
    portEXIT_CRITICAL(&s_lock);
    usb_hid_kick();
}

void kvm_input_tick(void)
{
    int64_t now = esp_timer_get_time();
    bool tripped = false;

    portENTER_CRITICAL(&s_lock);
    if (s.active.kind != KVM_SRC_NONE) {
        uint32_t age_ms = (uint32_t)((now - s.last_rx_us) / 1000);
        uint32_t wd = (s.active.kind == KVM_SRC_WS) ? s.wd_ws_ms : s.wd_udp_ms;
        if (wd > 0 && age_ms > wd) {
            if (anything_pressed_locked()) {
                s.st.wd_trips++;
                tripped = true;
            }
            release_all_locked();
        } else if (age_ms > SOURCE_IDLE_RELEASE_MS && !anything_pressed_locked()) {
            s.active.kind = KVM_SRC_NONE;
        }
    }
    if (now - s.rate_window_us >= 1000000) {
        s.st.rate_hz = (uint32_t)((uint64_t)s.rate_count * 1000000 / (now - s.rate_window_us));
        s.rate_count = 0;
        s.rate_window_us = now;
    }
    portEXIT_CRITICAL(&s_lock);

    if (tripped) {
        ESP_LOGW(TAG, "Link watchdog: no packets, all keys released");
        usb_hid_kick();
    }
}

void kvm_input_get_stats(kvm_stats_t *out)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    *out = s.st;
    out->active = s.active;
    out->active_age_ms = s.active.kind ? (uint32_t)((now - s.last_rx_us) / 1000) : 0;
    portEXIT_CRITICAL(&s_lock);
}

bool kvm_input_take_kbd(uint8_t report[8])
{
    bool r;
    portENTER_CRITICAL(&s_lock);
    r = s.kbd_dirty;
    if (r) {
        memcpy(report, s.kbd, 8);
        s.kbd_dirty = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return r;
}

static inline int32_t clamp_take(int32_t *acc, int32_t limit)
{
    int32_t v = *acc;
    if (v > limit) v = limit;
    if (v < -limit) v = -limit;
    *acc -= v;
    return v;
}

bool kvm_input_take_mouse(int32_t limit, uint8_t *buttons, int16_t *dx, int16_t *dy,
                          int8_t *wheel, int8_t *pan)
{
    bool r;
    portENTER_CRITICAL(&s_lock);
    r = s.mouse_dirty;
    if (r) {
        *buttons = s.buttons;
        *dx = (int16_t)clamp_take(&s.dx, limit);
        *dy = (int16_t)clamp_take(&s.dy, limit);
        *wheel = (int8_t)clamp_take(&s.wheel, 127);
        *pan = (int8_t)clamp_take(&s.pan, 127);
        s.mouse_dirty = s.dx || s.dy || s.wheel || s.pan;
    }
    portEXIT_CRITICAL(&s_lock);
    return r;
}

bool kvm_input_take_consumer(uint16_t *usage)
{
    bool r;
    portENTER_CRITICAL(&s_lock);
    r = s.consumer_dirty;
    if (r) {
        *usage = s.consumer;
        s.consumer_dirty = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return r;
}

bool kvm_input_wants_wakeup(void)
{
    bool r;
    portENTER_CRITICAL(&s_lock);
    r = (s.kbd_dirty || s.mouse_dirty) && anything_pressed_locked();
    portEXIT_CRITICAL(&s_lock);
    return r;
}

void kvm_input_discard_motion(void)
{
    portENTER_CRITICAL(&s_lock);
    s.dx = s.dy = s.wheel = s.pan = 0;
    portEXIT_CRITICAL(&s_lock);
}

void kvm_input_mark_all_dirty(void)
{
    portENTER_CRITICAL(&s_lock);
    s.kbd_dirty = s.mouse_dirty = s.consumer_dirty = true;
    portEXIT_CRITICAL(&s_lock);
}
