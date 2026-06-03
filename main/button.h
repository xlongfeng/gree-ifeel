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
 * @brief Initialize a GPIO button with interrupt-driven debounce.
 *
 * The GPIO is configured as input with internal pull-up. A falling-edge
 * interrupt feeds a FreeRTOS queue; a background task debounces presses
 * (50 ms window) and invokes @p on_press.
 *
 * @param gpio_num  GPIO number (e.g. GPIO_NUM_1)
 * @param on_press  Callback invoked from the button task on each confirmed press
 */
esp_err_t button_init(int gpio_num, button_cb_t on_press);

#ifdef __cplusplus
}
#endif
