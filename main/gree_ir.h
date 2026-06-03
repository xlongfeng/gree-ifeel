/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Operating modes */
#define GREE_MODE_AUTO 0
#define GREE_MODE_COOL 1
#define GREE_MODE_DRY 2
#define GREE_MODE_FAN 3
#define GREE_MODE_HEAT 4
#define GREE_MODE_ECONO 5

/* Fan levels (used for both BasicFan in frame 1 and Fan in frame 2) */
#define GREE_FAN_AUTO 0
#define GREE_FAN_LVL1 1 /* Quiet */
#define GREE_FAN_LVL2 2
#define GREE_FAN_LVL3 3
#define GREE_FAN_LVL4 4
#define GREE_FAN_LVL5 5 /* Turbo */

/* Vertical swing positions (byte[4] bits[3:0] of frame 1) */
#define GREE_SWING_V_LAST 0b0000
#define GREE_SWING_V_AUTO 0b0001
#define GREE_SWING_V_UP 0b0010
#define GREE_SWING_V_MID_UP 0b0011
#define GREE_SWING_V_MID 0b0100
#define GREE_SWING_V_MID_DOWN 0b0101
#define GREE_SWING_V_DOWN 0b0110
#define GREE_SWING_V_DOWN_AUTO 0b0111
#define GREE_SWING_V_MID_AUTO 0b1001
#define GREE_SWING_V_UP_AUTO 0b1011

/* Horizontal swing positions (byte[4] bits[6:4] of frame 1) */
#define GREE_SWING_H_OFF 0b000
#define GREE_SWING_H_AUTO 0b001
#define GREE_SWING_H_MAX_LEFT 0b010
#define GREE_SWING_H_LEFT 0b011
#define GREE_SWING_H_MID 0b100
#define GREE_SWING_H_RIGHT 0b101
#define GREE_SWING_H_MAX_RIGHT 0b110

/**
 * @brief Full GREE AC state.
 *
 * fan: use GREE_FAN_* constants (0–5).
 *   - When turbo=true, fan is forced to GREE_FAN_LVL5.
 *   - When quiet=true, fan is forced to GREE_FAN_LVL1.
 * temperature: 16–30 °C.
 */
typedef struct {
    bool power;
    uint8_t mode;        /* GREE_MODE_* */
    uint8_t temperature; /* 16–30 °C    */
    uint8_t fan;         /* GREE_FAN_*  */
    uint8_t swing_v;     /* GREE_SWING_V_* */
    uint8_t swing_h;     /* GREE_SWING_H_* */
    bool swing_auto;     /* auto vertical swing flag in byte[0] */
    bool turbo;
    bool sleep;
    bool quiet;
    bool light;
    bool xfan; /* keep fan running after stop */
    bool econo;
    bool ifeel;
    bool wifi;
    bool cooling_sensation;
} gree_ac_state_t;

/**
 * @brief Initialize the IR TX channel (38 kHz carrier).
 * @param gpio_num  GPIO number of the IR LED (e.g. GPIO_NUM_4)
 */
esp_err_t gree_ir_init(int gpio_num);

/**
 * @brief Encode and send a GREE command (two frames, ~400 ms total).
 * @param state  Desired AC state
 */
esp_err_t gree_ir_send(const gree_ac_state_t *state);

#ifdef __cplusplus
}
#endif
