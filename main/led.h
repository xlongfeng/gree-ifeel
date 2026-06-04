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

/**
 * @brief Initialize the LED GPIO as output (default off).
 * @param gpio_num  GPIO number (e.g. GPIO_NUM_8)
 */
esp_err_t led_init(int gpio_num);

/** @brief Turn the LED on (stays on indefinitely). */
void led_on(void);

/**
 * @brief Turn the LED on for a fixed duration, then auto off.
 * @param seconds  Duration in seconds before the LED turns off automatically.
 *                 Calling again before expiry restarts the timer.
 */
void led_on_for(uint32_t seconds);

/** @brief Turn the LED off (also cancels any pending auto-off timer). */
void led_off(void);

/** @brief Toggle the LED state. */
void led_toggle(void);

#ifdef __cplusplus
}
#endif
