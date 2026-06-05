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

#define DS18B20_GPIO_NUM CONFIG_IFEEL_DS18B20_GPIO
#define POWER_BUTTON_GPIO_NUM CONFIG_IFEEL_POWER_BUTTON_GPIO
#define TEMP_BUTTON_GPIO_NUM CONFIG_IFEEL_TEMP_BUTTON_GPIO
#define LIGHT_BUTTON_GPIO_NUM CONFIG_IFEEL_LIGHT_BUTTON_GPIO
#define GREE_IR_GPIO_NUM CONFIG_IFEEL_IR_GPIO
#define LED_GPIO_NUM CONFIG_IFEEL_LED_GPIO

void app_main(void)
{
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(led_init(LED_GPIO_NUM));
    ESP_ERROR_CHECK(gree_ir_init(GREE_IR_GPIO_NUM));
    ESP_ERROR_CHECK(ifeel_init());
    ESP_ERROR_CHECK(thermometer_init(DS18B20_GPIO_NUM, ifeel_on_temperature));
    ESP_ERROR_CHECK(button_init(POWER_BUTTON_GPIO_NUM, ifeel_button_pressed, NULL));
    ESP_ERROR_CHECK(button_init(TEMP_BUTTON_GPIO_NUM, ifeel_temperature_pressed, NULL));
    ESP_ERROR_CHECK(button_init(LIGHT_BUTTON_GPIO_NUM, ifeel_light_pressed, NULL));
}
