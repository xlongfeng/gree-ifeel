/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "led.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "led";

static int s_gpio_num = -1;
static int s_level = 1; /* active-low: 1 = off */

esp_err_t led_init(int gpio_num)
{
    s_gpio_num = gpio_num;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");
    gpio_set_level(gpio_num, 1); /* active-low: start with LED off */

    ESP_LOGI(TAG, "LED initialized on GPIO%d", gpio_num);
    return ESP_OK;
}

void led_on(void)
{
    s_level = 0;
    gpio_set_level(s_gpio_num, s_level);
}

void led_off(void)
{
    s_level = 1;
    gpio_set_level(s_gpio_num, s_level);
}

void led_toggle(void)
{
    s_level = !s_level;
    gpio_set_level(s_gpio_num, s_level);
}
