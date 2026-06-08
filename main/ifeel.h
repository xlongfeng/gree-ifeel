/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IFEEL_OFF = 0, /* AC off / powerup default — monitoring stopped */
    IFEEL_ON,      /* AC on, temperature monitoring active */
} ifeel_state_t;

/**
 * @brief Initialize the iFeel module (state = OFF, AC untouched).
 *        Installs the initial button dispatch handler.
 */
esp_err_t ifeel_init(void);

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
