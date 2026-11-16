/*
 * Единая точка приёма ввода: UDP (tailnet / LAN) и WebSocket веб-интерфейса.
 * Хранит текущее состояние клавиатуры/мыши, копит относительные движения,
 * реализует сторож связи, правило одного источника, фильтр порядка
 * пакетов (поле sequence) и счётчики.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    KVM_SRC_NONE = 0,
    KVM_SRC_UDP_TS,     /* UDP через tailnet (сокет MicroLink) */
    KVM_SRC_UDP_LAN,    /* UDP из локальной сети */
    KVM_SRC_WS,         /* WebSocket веб-интерфейса */
} kvm_src_kind_t;

typedef struct {
    kvm_src_kind_t kind;
    uint32_t ip;        /* IPv4, host byte order */
    uint16_t port;      /* UDP-порт отправителя или 0 */
    int      fd;        /* сокет WebSocket или -1 */
} kvm_source_t;

typedef enum {
    KVM_IN_OK = 0,
    KVM_IN_BAD,         /* неверный размер/magic/тип */
    KVM_IN_STALE,       /* старый или повторный sequence */
    KVM_IN_BUSY,        /* активен другой источник */
} kvm_in_result_t;

typedef struct {
    uint32_t rx_total;      /* принято корректных пакетов */
    uint32_t dropped;       /* отброшено всего */
    uint32_t dropped_bad;
    uint32_t dropped_stale;
    uint32_t dropped_busy;
    uint32_t dropped_denied;/* отправитель не в списке разрешённых */
    uint32_t wd_trips;      /* срабатывания сторожа (с отпусканием клавиш) */
    uint32_t rate_hz;       /* фактическая частота за последнюю секунду */
    kvm_source_t active;    /* текущий источник */
    uint32_t active_age_ms; /* сколько мс назад был последний пакет */
    int64_t  last_input_us; /* время последнего пакета (для светодиода) */
} kvm_stats_t;

void kvm_input_init(void);

/* Таймауты сторожа: для WebSocket и для UDP (0 — не отпускать по тишине). */
void kvm_input_set_watchdog(uint32_t ws_ms, uint32_t udp_ms);

/* Принять 16-байтный пакет протокола от источника. Потокобезопасно. */
kvm_in_result_t kvm_input_packet(const kvm_source_t *src, const uint8_t *data, size_t len);

/* Пакет отклонён фильтром отправителя. */
void kvm_input_count_denied(void);

/* Источник ушёл (закрыт WebSocket, потерян фокус): если он активен — отпустить всё. */
void kvm_input_release_source(const kvm_source_t *src);

/* Отпустить все клавиши и кнопки и освободить источник. */
void kvm_input_release_all(void);

/* Вызывается задачей HID каждые несколько мс: сторож и подсчёт частоты. */
void kvm_input_tick(void);

void kvm_input_get_stats(kvm_stats_t *out);
bool kvm_source_same(const kvm_source_t *a, const kvm_source_t *b);

/* ── Для задачи USB HID ─────────────────────────────────────────── */
bool kvm_input_take_kbd(uint8_t report[8]);
/* limit: 32767 для report-протокола, 127 для boot. */
bool kvm_input_take_mouse(int32_t limit, uint8_t *buttons, int16_t *dx, int16_t *dy,
                          int8_t *wheel, int8_t *pan);
bool kvm_input_take_consumer(uint16_t *usage);
/* Есть ли что-то, ради чего стоит будить хост (нажатие при USB suspend). */
bool kvm_input_wants_wakeup(void);
/* USB недоступен: сбросить накопленные движения, состояние клавиш сохранить. */
void kvm_input_discard_motion(void);
/* После (пере)подключения USB отправить текущее состояние заново. */
void kvm_input_mark_all_dirty(void);
