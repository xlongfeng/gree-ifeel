/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "led.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "led";

static int s_gpio_num = -1;
static int s_level = 1; /* active-low: 1 = off */
static esp_timer_handle_t s_auto_off_timer;

static void auto_off_cb(void *arg)
{
    led_off();
    ESP_LOGI(TAG, "LED auto off");
}

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

    esp_timer_create_args_t timer_args = {
        .callback = auto_off_cb,
        .name = "led_auto_off",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_auto_off_timer), TAG, "timer_create failed");

    ESP_LOGI(TAG, "LED initialized on GPIO%d", gpio_num);
    return ESP_OK;
}

void led_on(void)
{
    esp_timer_stop(s_auto_off_timer);
    s_level = 0;
    gpio_set_level(s_gpio_num, s_level);
}

void led_on_for(uint32_t seconds)
{
    led_on();
    esp_timer_start_once(s_auto_off_timer, (uint64_t)seconds * 1000000ULL);
    ESP_LOGI(TAG, "LED on for %lus", (unsigned long)seconds);
}

void led_off(void)
{
    esp_timer_stop(s_auto_off_timer);
    s_level = 1;
    gpio_set_level(s_gpio_num, s_level);
}

void led_toggle(void)
{
    s_level = !s_level;
    gpio_set_level(s_gpio_num, s_level);
}
