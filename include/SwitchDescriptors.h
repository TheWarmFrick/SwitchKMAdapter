#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SWITCH_ENDPOINT_SIZE 64

// Stick bounds (12-bit, 0..4095)
#define STICK_MIN    0x000
#define STICK_CENTER 0x800
#define STICK_MAX    0xFFF

// Pro Controller button bit definitions (3 bytes matching Report 0x30 bytes 3, 4, 5)
// Byte 0 (Report byte 3):
#define PROCON_BTN0_Y        (1U << 0)
#define PROCON_BTN0_X        (1U << 1)
#define PROCON_BTN0_B        (1U << 2)
#define PROCON_BTN0_A        (1U << 3)
#define PROCON_BTN0_SR_R     (1U << 4)
#define PROCON_BTN0_SL_R     (1U << 5)
#define PROCON_BTN0_R        (1U << 6)
#define PROCON_BTN0_ZR       (1U << 7)

// Byte 1 (Report byte 4):
#define PROCON_BTN1_MINUS    (1U << 0)
#define PROCON_BTN1_PLUS     (1U << 1)
#define PROCON_BTN1_RCLICK   (1U << 2)  // R3
#define PROCON_BTN1_LCLICK   (1U << 3)  // L3
#define PROCON_BTN1_HOME     (1U << 4)
#define PROCON_BTN1_CAPTURE  (1U << 5)
#define PROCON_BTN1_GRIP     (1U << 6)
#define PROCON_BTN1_CHG_GRIP (1U << 7)

// Byte 2 (Report byte 5):
#define PROCON_BTN2_DOWN     (1U << 0)
#define PROCON_BTN2_UP       (1U << 1)
#define PROCON_BTN2_RIGHT    (1U << 2)
#define PROCON_BTN2_LEFT     (1U << 3)
#define PROCON_BTN2_SR_L     (1U << 4)
#define PROCON_BTN2_SL_L     (1U << 5)
#define PROCON_BTN2_L        (1U << 6)
#define PROCON_BTN2_ZL       (1U << 7)

// Switch analog stick packing helper (12-bit X, 12-bit Y into 3 bytes)
static inline void procon_pack_stick(uint8_t *d, uint16_t x, uint16_t y) {
    d[0] = (uint8_t)(x & 0xFF);
    d[1] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    d[2] = (uint8_t)(y >> 4);
}

// Pro Controller full state struct
typedef struct {
    uint8_t btn[3];
    uint8_t stick[6];     // Left stick (0..2), Right stick (3..5) packed as 12-bit pairs
    int16_t imu[3][6];    // 3 frames x (accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z)
} ProconState;

typedef struct {
    uint8_t idx;
    ProconState state;
} ProconIdxState;