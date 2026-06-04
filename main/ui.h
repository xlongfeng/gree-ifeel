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

/** @brief Acquire the LVGL API mutex (must be held around all ui_label_* calls). */
void ui_lock(void);

/** @brief Release the LVGL API mutex. */
void ui_unlock(void);

/* ── Label stack ─────────────────────────────────────────────────────────── */

/**
 * @brief Opaque label identifier returned by ui_label_push().
 */
typedef int ui_label_id_t;

/**
 * @brief Add a new label at the bottom of the stack (lowest priority).
 *
 * Must be called with ui_lock held (or before the LVGL task starts).
 *
 * @param text  Initial label text
 * @return      Label ID to pass to other ui_label_* functions,
 *              or -1 if the stack is full.
 */
ui_label_id_t ui_label_push(const char *text);

/** @brief Show a label (include it in the rotation cycle). Must be called with ui_lock held. */
void ui_label_show(ui_label_id_t id);

/** @brief Hide a label (exclude it from the rotation cycle). Must be called with ui_lock held. */
void ui_label_hide(ui_label_id_t id);

/** @brief Update a label's text. Must be called with ui_lock held. */
void ui_label_set_text(ui_label_id_t id, const char *text);

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
