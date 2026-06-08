/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A button event describing which GPIO fired and the press type.
 */
typedef struct {
    int gpio_num;   /**< GPIO number of the button */
    bool long_press; /**< true = long press, false = short press */
} button_event_t;

/**
 * @brief Dispatch function type called for every button event.
 */
typedef void (*button_dispatch_fn_t)(button_event_t event);

/**
 * @brief Initialize a GPIO button.
 *
 * The GPIO is configured as input with internal pull-up. A falling-edge
 * interrupt feeds a FreeRTOS queue; a background task debounces presses
 * and classifies them:
 *   - Short press: released before BUTTON_LONG_PRESS_MS (1000 ms)
 *   - Long press: held for BUTTON_LONG_PRESS_MS — fires immediately at threshold
 *
 * Events are delivered via the dispatch function set with button_set_dispatch().
 *
 * @param gpio_num  GPIO number
 */
esp_err_t button_init(int gpio_num);

/**
 * @brief Push a dispatch function onto the handler stack.
 *
 * The pushed function becomes the active handler immediately.
 * Use when a new window is shown.
 *
 * @param fn  Dispatch function (must not be NULL)
 */
void button_push_dispatch(button_dispatch_fn_t fn);

/**
 * @brief Pop the top dispatch function from the handler stack.
 *
 * The previous handler becomes active. Has no effect if the stack is empty.
 * Use when a window is dismissed.
 */
void button_pop_dispatch(void);

#ifdef __cplusplus
}
#endif
