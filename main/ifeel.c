/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "ifeel.h"
#include "button.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gree_ir.h"
#include "ui.h"

#define IFEEL_SETPOINT_DEFAULT 27
#define IFEEL_SETPOINT_MIN 24
#define IFEEL_SETPOINT_MAX 28
#define IFEEL_MONITOR_INTERVAL_S 300LL /* seconds between adjustments (5 min) */

/* Temp limit config */
#define LIMIT_STEPS         6
#define LIMIT_INDEX_DEFAULT 3
#define LIMIT_LOW_BASE      23.4f /* LOW  at index 0 */
#define LIMIT_HIGH_BASE     25.0f /* HIGH at index 0 */
#define LIMIT_STRIDE        0.2f
#define LIMIT_AUTO_HIDE_US  8000000ULL /* 8 s */

/* Button GPIO numbers — read once at init */
#define F1_GPIO CONFIG_IFEEL_F1_BUTTON_GPIO
#define F2_GPIO CONFIG_IFEEL_F2_BUTTON_GPIO
#define F3_GPIO CONFIG_IFEEL_F3_BUTTON_GPIO

static const char *TAG = "ifeel";

static ifeel_state_t s_state = IFEEL_OFF;
static uint8_t s_setpoint = IFEEL_SETPOINT_DEFAULT;
static bool s_light = false;
static int64_t s_last_monitor_us = 0;

static int s_limit_index = LIMIT_INDEX_DEFAULT;
static esp_timer_handle_t s_limit_timer = NULL;

static float limit_low(void) { return LIMIT_LOW_BASE + s_limit_index * LIMIT_STRIDE; }
static float limit_high(void) { return LIMIT_HIGH_BASE + s_limit_index * LIMIT_STRIDE; }

/* ── Limit window helpers ─────────────────────────────────────────────────── */

static void limit_update_ui(void)
{
    char ht[16], lt[16];
    snprintf(ht, sizeof(ht), "HT: %.1f\xC2\xB0\x43", limit_high());
    snprintf(lt, sizeof(lt), "LT: %.1f\xC2\xB0\x43", limit_low());
    ui_set_ht(ht);
    ui_set_lt(lt);
}

static void limit_timer_cb(void *arg);  /* forward declaration */

/* ── AC helpers ───────────────────────────────────────────────────────────── */

static void ac_turn_on(void)
{
    gree_ac_state_t ac = {
        .power = true,
        .mode = GREE_MODE_COOL,
        .temperature = s_setpoint,
        .fan = GREE_FAN_AUTO,
        .light = s_light,
    };
    ESP_LOGI(TAG, "AC ON  setpoint=%d°C light=%d", s_setpoint, s_light);
    gree_ir_send(&ac);
}

static void ac_turn_off(void)
{
    gree_ac_state_t ac = {
        .power = false,
        .mode = GREE_MODE_COOL,
        .temperature = IFEEL_SETPOINT_DEFAULT,
        .fan = GREE_FAN_AUTO,
        .light = s_light,
    };
    ESP_LOGI(TAG, "AC OFF light=%d", s_light);
    gree_ir_send(&ac);
}

/* ── Per-window button handlers ───────────────────────────────────────────── */

static void handler_main(button_event_t ev);
static void handler_monitor(button_event_t ev);
static void handler_limit(button_event_t ev);

static void limit_show(void)
{
    limit_update_ui();
    ui_show_limit(true);
    esp_timer_stop(s_limit_timer);
    esp_timer_start_once(s_limit_timer, LIMIT_AUTO_HIDE_US);
    button_push_dispatch(handler_limit);
    ESP_LOGI(TAG, "Limit window shown (LOW=%.1f HIGH=%.1f)", limit_low(), limit_high());
}

static void limit_hide(void)
{
    esp_timer_stop(s_limit_timer);
    ui_show_limit(false);
    button_pop_dispatch();
    ESP_LOGI(TAG, "Limit window hidden");
}

static void limit_timer_cb(void *arg)
{
    ui_show_limit(false);
    button_pop_dispatch();
    ESP_LOGI(TAG, "Limit window auto-hidden");
}

static void enter_on(void)
{
    s_state = IFEEL_ON;
    s_setpoint = IFEEL_SETPOINT_DEFAULT;
    s_last_monitor_us = esp_timer_get_time();
    ac_turn_on();
    ui_set_bar(0, 0, IFEEL_MONITOR_INTERVAL_S);
    char buf[16];
    snprintf(buf, sizeof(buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
    ui_set_st(buf);
    ui_show_monitor(true);
    button_push_dispatch(handler_monitor);
    ESP_LOGI(TAG, "State → ON");
}

static void enter_off(void)
{
    s_state = IFEEL_OFF;
    ac_turn_off();
    ui_set_bar(0, 0, IFEEL_MONITOR_INTERVAL_S);
    ui_show_monitor(false);
    button_pop_dispatch();
    ESP_LOGI(TAG, "State → OFF");
}

/* Main window handler (OFF state) */
static void handler_main(button_event_t ev)
{
    if (is_button(ev, F1_GPIO) && is_short_pressed(ev)) {
        enter_on();
    } else if (is_button(ev, F2_GPIO) && is_long_pressed(ev)) {
        limit_show();
    } else if (is_button(ev, F3_GPIO) && is_short_pressed(ev)) {
        s_light = !s_light;
        ESP_LOGI(TAG, "F3: light → %d", s_light);
        gree_ac_state_t ac = {
            .power = false,
            .mode = GREE_MODE_COOL,
            .temperature = s_setpoint,
            .fan = GREE_FAN_AUTO,
            .light = s_light,
        };
        gree_ir_send(&ac);
    }
}

/* Monitor window handler (ON state) */
static void handler_monitor(button_event_t ev)
{
    if (is_button(ev, F1_GPIO) && is_short_pressed(ev)) {
        enter_off();
    } else if (is_button(ev, F2_GPIO) && is_short_pressed(ev)) {
        s_setpoint = (s_setpoint >= IFEEL_SETPOINT_MAX) ? IFEEL_SETPOINT_MIN : s_setpoint + 1;
        ESP_LOGI(TAG, "F2: setpoint → %d°C", s_setpoint);
        char buf[16];
        snprintf(buf, sizeof(buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
        ui_set_st(buf);
        ac_turn_on();
    } else if (is_button(ev, F2_GPIO) && is_long_pressed(ev)) {
        limit_show();
    } else if (is_button(ev, F3_GPIO) && is_short_pressed(ev)) {
        s_light = !s_light;
        ESP_LOGI(TAG, "F3: light → %d", s_light);
        gree_ac_state_t ac = {
            .power = true,
            .mode = GREE_MODE_COOL,
            .temperature = s_setpoint,
            .fan = GREE_FAN_AUTO,
            .light = s_light,
        };
        gree_ir_send(&ac);
    }
}

/* Limit config window handler */
static void handler_limit(button_event_t ev)
{
    if (is_button(ev, F2_GPIO) && is_short_pressed(ev)) {
        s_limit_index = (s_limit_index + 1) % LIMIT_STEPS;
        limit_update_ui();
        esp_timer_stop(s_limit_timer);
        esp_timer_start_once(s_limit_timer, LIMIT_AUTO_HIDE_US);
        ESP_LOGI(TAG, "Limit cycle → index=%d LOW=%.1f HIGH=%.1f",
                 s_limit_index, limit_low(), limit_high());
    } else if (is_button(ev, F2_GPIO) && is_long_pressed(ev)) {
        limit_hide();
    }
    /* All other buttons ignored in limit window */
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t ifeel_init(void)
{
    s_state = IFEEL_OFF;
    s_setpoint = IFEEL_SETPOINT_DEFAULT;
    s_last_monitor_us = 0;

    esp_timer_create_args_t timer_args = {
        .callback = limit_timer_cb,
        .name = "limit_auto_hide",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_limit_timer), TAG, "limit timer create failed");

    button_push_dispatch(handler_main);
    ESP_LOGI(TAG, "State → OFF (AC untouched)");
    return ESP_OK;
}

void ifeel_on_temperature(float temperature)
{
    /* Always update the room temperature label */
    char rt_buf[16];
    snprintf(rt_buf, sizeof(rt_buf), "RT: %.1f\xC2\xB0\x43", temperature);
    ui_set_rt(rt_buf);

    if (s_state != IFEEL_ON) {
        return;
    }

    int64_t now = esp_timer_get_time();
    int64_t elapsed_s = (now - s_last_monitor_us) / 1000000LL;
    ESP_LOGI(TAG, "tick elapsed=%llds", elapsed_s);

    /* Update progress bar (clamped to monitor interval) */
    int64_t bar_val = elapsed_s < IFEEL_MONITOR_INTERVAL_S ? elapsed_s : IFEEL_MONITOR_INTERVAL_S;
    ui_set_bar((int)bar_val, 0, IFEEL_MONITOR_INTERVAL_S);

    if (elapsed_s < IFEEL_MONITOR_INTERVAL_S) {
        return;
    }
    s_last_monitor_us = now;

    uint8_t new_setpoint = s_setpoint;

    if (temperature > limit_high() && s_setpoint > IFEEL_SETPOINT_MIN) {
        new_setpoint = s_setpoint - 1;
    } else if (temperature < limit_low() && s_setpoint < IFEEL_SETPOINT_MAX) {
        new_setpoint = s_setpoint + 1;
    }

    if (new_setpoint != s_setpoint) {
        s_setpoint = new_setpoint;
        ESP_LOGI(TAG, "Adjust setpoint → %d°C (room=%.1f°C)", s_setpoint, temperature);
        char st_buf[16];
        snprintf(st_buf, sizeof(st_buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
        ui_set_st(st_buf);
        ac_turn_on();
    } else {
        ESP_LOGI(TAG, "No adjustment (room=%.1f°C setpoint=%d°C)", temperature, s_setpoint);
    }
}

ifeel_state_t ifeel_get_state(void) { return s_state; }

uint8_t ifeel_get_setpoint(void) { return s_setpoint; }
