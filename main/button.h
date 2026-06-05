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

typedef void (*button_cb_t)(void);

/**
 * @brief Initialize a GPIO button with short and long press support.
 *
 * The GPIO is configured as input with internal pull-up. A falling-edge
 * interrupt feeds a FreeRTOS queue; a background task debounces presses
 * and classifies them:
 *   - Short press: released before BUTTON_LONG_PRESS_MS (1000 ms) →
 *     @p on_short_press is called on release.
 *   - Long press: held for BUTTON_LONG_PRESS_MS → @p on_long_press is
 *     called immediately at the threshold (not on release).
 *
 * Either callback may be NULL if not needed.
 *
 * @param gpio_num       GPIO number
 * @param on_short_press Callback for short press (may be NULL)
 * @param on_long_press  Callback for long press (may be NULL)
 */
esp_err_t button_init(int gpio_num, button_cb_t on_short_press, button_cb_t on_long_press);

#ifdef __cplusplus
}
#endif
