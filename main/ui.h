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

/**
 * @brief Initialize I2C, SSD1306 panel, LVGL, tick timer, LVGL task, and UI.
 */
esp_err_t ui_init(void);

/**
 * @brief Switch between main window (OFF) and monitor window (ON).
 * @param show  true → monitor foreground; false → main foreground
 */
void ui_show_monitor(bool show);

/**
 * @brief Bring or send the limit-config window to/from the foreground.
 */
void ui_show_limit(bool show);

/**
 * @brief Set the high-limit label in the limit window (e.g. "HT: 25.6°C").
 */
void ui_set_ht(const char *text);

/**
 * @brief Set the low-limit label in the limit window (e.g. "LT: 23.6°C").
 */
void ui_set_lt(const char *text);

/**
 * @brief Set the setpoint label in the monitor window.
 */
void ui_set_st(const char *text);

/**
 * @brief Set the room-temperature label in both windows.
 */
void ui_set_rt(const char *text);

/**
 * @brief Set the progress bar value in the monitor window.
 */
void ui_set_bar(int value, int min, int max);

/**
 * @brief Show or hide the message dialog (overlays all windows).
 */
void ui_show_msg(bool show);

/**
 * @brief Set the message text in the dialog (center-aligned).
 */
void ui_set_msg(const char *text);

#ifdef __cplusplus
}
#endif
