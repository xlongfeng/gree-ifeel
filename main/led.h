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

/**
 * @brief Initialize the LED GPIO as output (default off).
 * @param gpio_num  GPIO number (e.g. GPIO_NUM_8)
 */
esp_err_t led_init(int gpio_num);

/** @brief Turn the LED on. */
void led_on(void);

/** @brief Turn the LED off. */
void led_off(void);

/** @brief Toggle the LED state. */
void led_toggle(void);

#ifdef __cplusplus
}
#endif
