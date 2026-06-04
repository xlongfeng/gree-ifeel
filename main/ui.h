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

/* ── Labels ──────────────────────────────────────────────────────────────── */

/**
 * @brief Set the top area label text.
 *        Shows "GREE iFeel" when iFeel is OFF, setpoint when ON.
 */
void ui_set_top_label(const char *text);

/**
 * @brief Set the mid area label text (room temperature).
 */
void ui_set_mid_label(const char *text);

/* ── Lower area ──────────────────────────────────────────────────────────── */

/** @brief Set the LED indicator state (right side of bottom area, blinks at 1s when on). */
void ui_set_led_indicator(bool on);

/**
 * @brief Set the progress bar value (left side of bottom area, 50px wide).
 * @param value  Current value
 * @param min    Minimum value
 * @param max    Maximum value
 */
void ui_set_bar(int value, int min, int max);

#ifdef __cplusplus
}
#endif
