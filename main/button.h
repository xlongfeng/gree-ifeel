/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Application-defined key codes delivered via LV_EVENT_KEY.
 *
 * These are chosen outside LVGL's reserved range (0x00–0x7F) to avoid
 * collisions with LV_KEY_ENTER (10), LV_KEY_NEXT (9), LV_KEY_PREV (11), etc.
 */
#define LV_KEY_BUTTON_0 0x1000U     /**< Button 0 click (short press) */
#define LV_KEY_ALT_BUTTON_0 0x1001U /**< Button 0 hold  (long press)  */
#define LV_KEY_BUTTON_1 0x1002U     /**< Button 1 click (short press) */
#define LV_KEY_ALT_BUTTON_1 0x1003U /**< Button 1 hold  (long press)  */
#define LV_KEY_BUTTON_2 0x1004U     /**< Button 2 click (short press) */
#define LV_KEY_ALT_BUTTON_2 0x1005U /**< Button 2 hold  (long press)  */

/**
 * @brief Register a GPIO button.
 *
 * Configures the GPIO as input with internal pull-up.
 * Must be called before button_indev_create().
 *
 * @param gpio_num  GPIO number (active LOW)
 * @param key       Key code to fire on a click  (LV_KEY_BUTTON_x)
 * @param hold      Key code to fire on a hold   (LV_KEY_ALT_BUTTON_x)
 */
esp_err_t button_init(int gpio_num, uint32_t key, uint32_t hold);

/**
 * @brief Create the LVGL keypad indev and start polling all registered buttons.
 *
 * Must be called after lv_init() (i.e., after ui_init()) and after all
 * button_init() calls.
 *
 * @return The created lv_indev_t pointer (never NULL on success).
 */
lv_indev_t *button_indev_create(void);

/**
 * @brief Return the indev created by button_indev_create().
 */
lv_indev_t *button_get_indev(void);

#ifdef __cplusplus
}
#endif
