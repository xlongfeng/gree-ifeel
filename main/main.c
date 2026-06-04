/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "button.h"
#include "driver/gpio.h"
#include "gree_ir.h"
#include "ifeel.h"
#include "led.h"
#include "thermometer.h"
#include "ui.h"

#define DS18B20_GPIO_NUM GPIO_NUM_7
#define POWER_BUTTON_GPIO_NUM GPIO_NUM_3
#define TEMP_BUTTON_GPIO_NUM GPIO_NUM_4
#define LIGHT_BUTTON_GPIO_NUM GPIO_NUM_10
#define GREE_IR_GPIO_NUM GPIO_NUM_0
#define LED_GPIO_NUM GPIO_NUM_8

void app_main(void)
{
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(led_init(LED_GPIO_NUM));
    ESP_ERROR_CHECK(gree_ir_init(GREE_IR_GPIO_NUM));
    ESP_ERROR_CHECK(ifeel_init());
    ESP_ERROR_CHECK(thermometer_init(DS18B20_GPIO_NUM, ifeel_on_temperature));
    ESP_ERROR_CHECK(button_init(POWER_BUTTON_GPIO_NUM, ifeel_button_pressed));
    ESP_ERROR_CHECK(button_init(TEMP_BUTTON_GPIO_NUM, ifeel_temperature_pressed));
    ESP_ERROR_CHECK(button_init(LIGHT_BUTTON_GPIO_NUM, ifeel_light_pressed));
}
