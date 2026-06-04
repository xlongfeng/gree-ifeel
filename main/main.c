/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>

#include "button.h"
#include "driver/gpio.h"
#include "ds18b20.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gree_ir.h"
#include "ifeel.h"
#include "led.h"
#include "onewire_bus.h"
#include "ui.h"

static const char *TAG = "thermostatic";

#define DS18B20_GPIO_NUM GPIO_NUM_7
#define POWER_BUTTON_GPIO_NUM GPIO_NUM_3
#define TEMP_BUTTON_GPIO_NUM GPIO_NUM_4
#define LIGHT_BUTTON_GPIO_NUM GPIO_NUM_10
#define GREE_IR_GPIO_NUM GPIO_NUM_0
#define LED_GPIO_NUM GPIO_NUM_8

static void temperature_task(void *arg)
{
    ds18b20_device_handle_t s_ds18b20 = NULL;

    ESP_LOGI(TAG, "Initialize DS18B20 on GPIO%d", DS18B20_GPIO_NUM);
    onewire_bus_handle_t onewire_bus = NULL;
    onewire_bus_config_t onewire_bus_config = {
        .bus_gpio_num = DS18B20_GPIO_NUM,
    };
    onewire_bus_rmt_config_t onewire_rmt_config = {
        .max_rx_bytes = 10,
    };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&onewire_bus_config, &onewire_rmt_config, &onewire_bus));

    ds18b20_config_t ds_cfg = {};
    if (ds18b20_new_device_from_bus(onewire_bus, &ds_cfg, &s_ds18b20) == ESP_OK) {
        ESP_LOGI(TAG, "DS18B20 found on GPIO%d", DS18B20_GPIO_NUM);
    } else {
        ESP_LOGW(TAG, "No DS18B20 device found on GPIO%d", DS18B20_GPIO_NUM);
    }

    while (1) {
        if (s_ds18b20 != NULL) {
            float temperature = 0.0f;
            if (ds18b20_trigger_temperature_conversion(s_ds18b20) == ESP_OK &&
                ds18b20_get_temperature(s_ds18b20, &temperature) == ESP_OK) {
                ESP_LOGI(TAG, "Temperature: %.1f C", temperature);
                ifeel_on_temperature(temperature);
                ui_lock();
                lvgl_set_temperature(temperature);
                ui_unlock();
            } else {
                ESP_LOGW(TAG, "Failed to read DS18B20 temperature");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(ui_init());

    xTaskCreate(temperature_task, "ds18b20", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(led_init(LED_GPIO_NUM));
    ESP_ERROR_CHECK(gree_ir_init(GREE_IR_GPIO_NUM));
    ESP_ERROR_CHECK(button_init(POWER_BUTTON_GPIO_NUM, ifeel_button_pressed));
    ESP_ERROR_CHECK(button_init(TEMP_BUTTON_GPIO_NUM, ifeel_temperature_pressed));
    ESP_ERROR_CHECK(button_init(LIGHT_BUTTON_GPIO_NUM, ifeel_light_pressed));
    ESP_ERROR_CHECK(ifeel_init());
}
