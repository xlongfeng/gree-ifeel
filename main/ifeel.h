/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IFEEL_OFF = 0, /* AC off / powerup default — monitoring stopped */
    IFEEL_ON,      /* AC on, temperature monitoring active */
} ifeel_state_t;

/**
 * @brief Initialize the iFeel module (state = OFF, AC untouched).
 */
esp_err_t ifeel_init(void);

/**
 * @brief Handle a button press. Transitions:
 *   OFF → ON
 *   ON  → OFF
 */
void ifeel_button_pressed(void);

/**
 * @brief Handle the temperature button press.
 *
 * Normal: only active in ON state — increments setpoint, wraps, resends AC command.
 * When limit window visible: cycles limit index up (wraps), restarts 3s auto-hide timer.
 */
void ifeel_temperature_pressed(void);

/**
 * @brief Handle the temperature button long press.
 *        No-op in all states (registered for completeness).
 */
void ifeel_temperature_long_pressed(void);

/**
 * @brief Handle the light button press.
 *
 * Toggles the AC display light and resends the IR command.
 * Ignored when the limit-config window is visible.
 */
void ifeel_light_pressed(void);

/**
 * @brief Handle the light button long press.
 *        Shows the limit-config window; hides it if already visible.
 */
void ifeel_light_long_pressed(void);

/**
 * @brief Feed the latest room temperature reading.
 *
 * Must be called every second from the DS18B20 task.
 * In ON state, fires an AC setpoint adjustment every 5 minutes.
 *
 * @param temperature  Room temperature in °C
 */
void ifeel_on_temperature(float temperature);

/**
 * @brief Return the current iFeel state.
 */
ifeel_state_t ifeel_get_state(void);

/**
 * @brief Return the current AC setpoint (°C). Valid in ON state.
 */
uint8_t ifeel_get_setpoint(void);

#ifdef __cplusplus
}
#endif
