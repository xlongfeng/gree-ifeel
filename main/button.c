/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "button.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BUTTON_DEBOUNCE_MS 50

static const char *TAG = "button";

static QueueHandle_t s_evt_queue;
static button_cb_t s_on_press;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(s_evt_queue, &gpio_num, NULL);
}

static void button_task(void *arg)
{
    uint32_t gpio_num;
    while (1) {
        if (xQueueReceive(s_evt_queue, &gpio_num, portMAX_DELAY)) {
            /* Debounce: wait, then confirm the pin is still low */
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(gpio_num) == 0) {
                ESP_LOGI(TAG, "Button pressed on GPIO%lu", gpio_num);
                if (s_on_press) {
                    s_on_press();
                }
                /* Wait for release before re-arming */
                while (gpio_get_level(gpio_num) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            /* Drain any events that accumulated during debounce */
            xQueueReset(s_evt_queue);
        }
    }
}

esp_err_t button_init(int gpio_num, button_cb_t on_press)
{
    s_on_press = on_press;
    s_evt_queue = xQueueCreate(4, sizeof(uint32_t));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "isr_service failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(gpio_num, gpio_isr_handler, (void *)(intptr_t)gpio_num), TAG,
                        "isr_handler_add failed");

    xTaskCreate(button_task, "button", 2048, NULL, 10, NULL);

    ESP_LOGI(TAG, "Button initialized on GPIO%d", gpio_num);
    return ESP_OK;
}
