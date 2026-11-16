/*
 * Сетевой протокол esp32-kvm-ip (без изменений): 16 байт, little-endian.
 * Используется и для UDP, и для бинарных сообщений WebSocket веб-интерфейса.
 */
#pragma once
#include <stdint.h>

#define KVM_PACKET_MAGIC 0xCAFE
#define KVM_PACKET_SIZE  16

enum {
    KVM_EVT_MOUSE    = 0x01,
    KVM_EVT_KEYBOARD = 0x02,
    KVM_EVT_CONSUMER = 0x03,
};

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint32_t sequence;
    uint8_t  type;
    uint8_t  _reserved;
    union {
        struct __attribute__((packed)) {
            uint8_t buttons;
            int16_t dx;
            int16_t dy;
            int8_t  wheel;
            int8_t  pan;
            uint8_t _pad;
        } mouse;
        struct __attribute__((packed)) {
            uint8_t modifiers;
            uint8_t reserved;
            uint8_t keycodes[6];
        } keyboard;
        struct __attribute__((packed)) {
            uint16_t usage_id;
            uint8_t  _pad[6];
        } consumer;
    };
} kvm_packet_t;

_Static_assert(sizeof(kvm_packet_t) == KVM_PACKET_SIZE, "packet must be 16 bytes");
