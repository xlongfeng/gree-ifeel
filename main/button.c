/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "button.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui.h"

#define BUTTON_MAX 4
#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_HOLD_MS 1000
#define BUTTON_READ_PERIOD_MS 20

static const char *TAG = "button";

typedef struct {
    int gpio;
    uint32_t key;
    uint32_t hold;
    int64_t press_time_us; /* esp_timer_get_time() when first pressed; 0 = not pressed */
    bool hold_fired;
} btn_entry_t;

static btn_entry_t s_btns[BUTTON_MAX];
static int s_btn_count = 0;
static lv_indev_t *s_indev = NULL;

/* Pending key event queue: each entry holds a {key, state} pair */
#define PENDING_SIZE 8
typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} pending_entry_t;

static pending_entry_t s_pending[PENDING_SIZE];
static int s_pending_head = 0;
static int s_pending_tail = 0;

static bool pending_empty(void) { return s_pending_head == s_pending_tail; }

static void pending_push(uint32_t key, lv_indev_state_t state)
{
    int next = (s_pending_tail + 1) % PENDING_SIZE;
    if (next != s_pending_head) {
        s_pending[s_pending_tail].key = key;
        s_pending[s_pending_tail].state = state;
        s_pending_tail = next;
    }
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* Drain the pending queue one entry per call */
    if (!pending_empty()) {
        data->key = s_pending[s_pending_head].key;
        data->state = s_pending[s_pending_head].state;
        s_pending_head = (s_pending_head + 1) % PENDING_SIZE;
        data->continue_reading = !pending_empty();
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;

    int64_t now = esp_timer_get_time();

    for (int i = 0; i < s_btn_count; i++) {
        bool pressed = (gpio_get_level(s_btns[i].gpio) == 0);

        if (pressed) {
            if (s_btns[i].press_time_us == 0) {
                s_btns[i].press_time_us = now;
                s_btns[i].hold_fired = false;
            } else if (!s_btns[i].hold_fired) {
                int64_t elapsed_ms = (now - s_btns[i].press_time_us) / 1000LL;
                if (elapsed_ms >= BUTTON_HOLD_MS) {
                    s_btns[i].hold_fired = true;
                    ESP_LOGI(TAG, "Hold on GPIO%d", s_btns[i].gpio);
                    pending_push(s_btns[i].hold, LV_INDEV_STATE_PRESSED);
                    return;
                }
            }
        } else {
            if (s_btns[i].press_time_us != 0) {
                int64_t elapsed_ms = (now - s_btns[i].press_time_us) / 1000LL;
                bool hold_was_fired = s_btns[i].hold_fired;
                s_btns[i].press_time_us = 0;
                s_btns[i].hold_fired = false;
                if (elapsed_ms >= BUTTON_DEBOUNCE_MS) {
                    if (!hold_was_fired) {
                        ESP_LOGI(TAG, "Click on GPIO%d", s_btns[i].gpio);
                        pending_push(s_btns[i].key, LV_INDEV_STATE_PRESSED);
                        pending_push(s_btns[i].key, LV_INDEV_STATE_RELEASED);
                    } else {
                        ESP_LOGI(TAG, "Release on GPIO%d", s_btns[i].gpio);
                        pending_push(s_btns[i].hold, LV_INDEV_STATE_RELEASED);
                    }
                    return;
                }
            }
        }
    }
}

esp_err_t button_init(int gpio_num, uint32_t key, uint32_t hold)
{
    if (s_btn_count >= BUTTON_MAX) {
        ESP_LOGE(TAG, "Max buttons (%d) reached", BUTTON_MAX);
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

    s_btns[s_btn_count].gpio = gpio_num;
    s_btns[s_btn_count].key = key;
    s_btns[s_btn_count].hold = hold;
    s_btns[s_btn_count].press_time_us = 0;
    s_btns[s_btn_count].hold_fired = false;
    s_btn_count++;

    ESP_LOGI(TAG, "Button initialized on GPIO%d (click=0x%lx hold=0x%lx)", gpio_num, (unsigned long)key,
             (unsigned long)hold);
    return ESP_OK;
}

lv_indev_t *button_indev_create(void)
{
    ui_lock();
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_indev, read_cb);
    lv_timer_t *read_timer = lv_indev_get_read_timer(s_indev);
    lv_timer_set_period(read_timer, BUTTON_READ_PERIOD_MS);
    ui_unlock();

    ESP_LOGI(TAG, "Button indev created (%d buttons, read period %dms)", s_btn_count, BUTTON_READ_PERIOD_MS);
    return s_indev;
}

lv_indev_t *button_get_indev(void) { return s_indev; }
