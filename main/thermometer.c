/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "thermometer.h"

#include "ds18b20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"

static const char *TAG = "thermometer";

typedef struct {
    int gpio_num;
    thermometer_cb_t on_temperature;
} thermometer_task_arg_t;

static void thermometer_task(void *arg)
{
    thermometer_task_arg_t *cfg = (thermometer_task_arg_t *)arg;

    ESP_LOGI(TAG, "Initialize DS18B20 on GPIO%d", cfg->gpio_num);
    onewire_bus_handle_t onewire_bus = NULL;
    onewire_bus_config_t onewire_bus_config = {
        .bus_gpio_num = cfg->gpio_num,
    };
    onewire_bus_rmt_config_t onewire_rmt_config = {
        .max_rx_bytes = 10,
    };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&onewire_bus_config, &onewire_rmt_config, &onewire_bus));

    ds18b20_device_handle_t ds18b20 = NULL;
    ds18b20_config_t ds_cfg = {};
    if (ds18b20_new_device_from_bus(onewire_bus, &ds_cfg, &ds18b20) == ESP_OK) {
        ESP_LOGI(TAG, "DS18B20 found on GPIO%d", cfg->gpio_num);
    } else {
        ESP_LOGW(TAG, "No DS18B20 found on GPIO%d", cfg->gpio_num);
    }

    while (1) {
        if (ds18b20 != NULL) {
            float temperature = 0.0f;
            if (ds18b20_trigger_temperature_conversion(ds18b20) == ESP_OK &&
                ds18b20_get_temperature(ds18b20, &temperature) == ESP_OK) {
                ESP_LOGI(TAG, "Temperature: %.1f C", temperature);
                if (cfg->on_temperature) {
                    cfg->on_temperature(temperature);
                }
            } else {
                ESP_LOGW(TAG, "Failed to read temperature");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t thermometer_init(int gpio_num, thermometer_cb_t on_temperature)
{
    thermometer_task_arg_t *arg = (thermometer_task_arg_t *)malloc(sizeof(thermometer_task_arg_t));
    if (!arg) {
        return ESP_ERR_NO_MEM;
    }
    arg->gpio_num = gpio_num;
    arg->on_temperature = on_temperature;

    xTaskCreate(thermometer_task, "thermometer", 4096, arg, 5, NULL);
    return ESP_OK;
}
