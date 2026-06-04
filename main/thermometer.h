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
 * @brief Callback invoked each second with the latest temperature reading.
 * @param temperature  Room temperature in °C
 */
typedef void (*thermometer_cb_t)(float temperature);

/**
 * @brief Initialize the DS18B20 one-wire bus and start the reading task.
 *
 * Spawns a FreeRTOS task that reads temperature every second and calls
 * @p on_temperature with the result.
 *
 * @param gpio_num       GPIO the DS18B20 data line is connected to
 * @param on_temperature Callback invoked on each successful reading
 */
esp_err_t thermometer_init(int gpio_num, thermometer_cb_t on_temperature);

#ifdef __cplusplus
}
#endif
