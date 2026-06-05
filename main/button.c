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

#define BUTTON_MAX 4
#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_LONG_PRESS_MS 1000

static const char *TAG = "button";

typedef struct {
    int gpio_num;
    button_cb_t on_short_press;
    button_cb_t on_long_press;
} button_entry_t;

static button_entry_t s_buttons[BUTTON_MAX];
static int s_button_count = 0;
static QueueHandle_t s_evt_queue;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(s_evt_queue, &gpio_num, NULL);
}

static button_entry_t *find_button(int gpio_num)
{
    for (int i = 0; i < s_button_count; i++) {
        if (s_buttons[i].gpio_num == gpio_num) {
            return &s_buttons[i];
        }
    }
    return NULL;
}

static void button_task(void *arg)
{
    uint32_t gpio_num;
    while (1) {
        if (xQueueReceive(s_evt_queue, &gpio_num, portMAX_DELAY)) {
            /* Debounce: wait, then confirm pin is still low */
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(gpio_num) == 0) {
                button_entry_t *btn = find_button((int)gpio_num);
                bool long_fired = false;
                int held_ms = 0;

                /* Poll until release, firing long press at threshold */
                while (gpio_get_level(gpio_num) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    held_ms += 10;
                    if (!long_fired && held_ms >= BUTTON_LONG_PRESS_MS) {
                        long_fired = true;
                        ESP_LOGI(TAG, "Long press on GPIO%lu", gpio_num);
                        if (btn && btn->on_long_press) {
                            btn->on_long_press();
                        }
                        break; /* stop polling after long press fires */
                    }
                }

                if (!long_fired) {
                    ESP_LOGI(TAG, "Short press on GPIO%lu", gpio_num);
                    if (btn && btn->on_short_press) {
                        btn->on_short_press();
                    }
                } else {
                    /* Wait for physical release before re-arming */
                    while (gpio_get_level(gpio_num) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
            }

            /* Drain accumulated events for this GPIO */
            uint32_t discard;
            while (xQueuePeek(s_evt_queue, &discard, 0) == pdTRUE && discard == gpio_num) {
                xQueueReceive(s_evt_queue, &discard, 0);
            }
        }
    }
}

esp_err_t button_init(int gpio_num, button_cb_t on_short_press, button_cb_t on_long_press)
{
    if (s_button_count >= BUTTON_MAX) {
        ESP_LOGE(TAG, "Max buttons (%d) reached", BUTTON_MAX);
        return ESP_ERR_NO_MEM;
    }

    /* Create shared queue, task, and ISR service on first call */
    if (s_button_count == 0) {
        s_evt_queue = xQueueCreate(8, sizeof(uint32_t));
        xTaskCreate(button_task, "button", 2048, NULL, 10, NULL);
        ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "isr_service failed");
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(gpio_num, gpio_isr_handler, (void *)(intptr_t)gpio_num), TAG,
                        "isr_handler_add failed");

    s_buttons[s_button_count].gpio_num = gpio_num;
    s_buttons[s_button_count].on_short_press = on_short_press;
    s_buttons[s_button_count].on_long_press = on_long_press;
    s_button_count++;

    ESP_LOGI(TAG, "Button initialized on GPIO%d", gpio_num);
    return ESP_OK;
}
