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
 * @brief Initialize I2C, SSD1306 panel, LVGL, tick timer, LVGL task, and UI.
 */
esp_err_t ui_init(void);

/** @brief Acquire the LVGL API mutex (must be held around all lv_* calls). */
void ui_lock(void);

/** @brief Release the LVGL API mutex. */
void ui_unlock(void);

/** @brief Update the room temperature label. Call with ui_lock held. */
void lvgl_set_temperature(float temperature);

#ifdef __cplusplus
}
#endif
