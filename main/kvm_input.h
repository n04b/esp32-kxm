/*
 * Single input entry point: UDP (tailnet / LAN) and the web UI WebSocket.
 * Holds the current keyboard/mouse state, accumulates relative motion,
 * implements the link watchdog, the single-source rule, the packet ordering
 * filter (sequence field) and counters.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    KVM_SRC_NONE = 0,
    KVM_SRC_UDP_TS,     /* UDP over tailnet (MicroLink socket) */
    KVM_SRC_UDP_LAN,    /* UDP from the local network */
    KVM_SRC_WS,         /* web UI WebSocket */
} kvm_src_kind_t;

typedef struct {
    kvm_src_kind_t kind;
    uint32_t ip;        /* IPv4, host byte order */
    uint16_t port;      /* sender UDP port or 0 */
    int      fd;        /* WebSocket socket or -1 */
} kvm_source_t;

typedef enum {
    KVM_IN_OK = 0,
    KVM_IN_BAD,         /* bad size/magic/type */
    KVM_IN_STALE,       /* old or duplicate sequence */
    KVM_IN_BUSY,        /* another source is active */
} kvm_in_result_t;

typedef struct {
    uint32_t rx_total;      /* valid packets received */
    uint32_t dropped;       /* total dropped */
    uint32_t dropped_bad;
    uint32_t dropped_stale;
    uint32_t dropped_busy;
    uint32_t dropped_denied;/* sender not in the allowlist */
    uint32_t wd_trips;      /* watchdog trips (with key release) */
    uint32_t rate_hz;       /* actual rate over the last second */
    kvm_source_t active;    /* current source */
    uint32_t active_age_ms; /* ms since the last packet */
    int64_t  last_input_us; /* time of the last packet (for the LED) */
} kvm_stats_t;

void kvm_input_init(void);

/* Watchdog timeouts for WebSocket and UDP (0 = never release on silence). */
void kvm_input_set_watchdog(uint32_t ws_ms, uint32_t udp_ms);

/* Accept a 16-byte protocol packet from a source. Thread-safe. */
kvm_in_result_t kvm_input_packet(const kvm_source_t *src, const uint8_t *data, size_t len);

/* Packet rejected by the sender filter. */
void kvm_input_count_denied(void);

/* Source went away (WebSocket closed, focus lost): if it is active, release everything. */
void kvm_input_release_source(const kvm_source_t *src);

/* Release all keys and buttons and free the source. */
void kvm_input_release_all(void);

/* Called by the HID task every few ms: watchdog and rate counting. */
void kvm_input_tick(void);

void kvm_input_get_stats(kvm_stats_t *out);
bool kvm_source_same(const kvm_source_t *a, const kvm_source_t *b);

/* ── For the USB HID task ───────────────────────────────────────── */
bool kvm_input_take_kbd(uint8_t report[8]);
/* limit: 32767 for the report protocol, 127 for boot. */
bool kvm_input_take_mouse(int32_t limit, uint8_t *buttons, int16_t *dx, int16_t *dy,
                          int8_t *wheel, int8_t *pan);
bool kvm_input_take_consumer(uint16_t *usage);
/* Whether there is anything worth waking the host for (a press during USB suspend). */
bool kvm_input_wants_wakeup(void);
/* USB unavailable: drop accumulated motion, keep the key state. */
void kvm_input_discard_motion(void);
/* After USB (re)connect, resend the current state. */
void kvm_input_mark_all_dirty(void);
