/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "ifeel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gree_ir.h"
#include "led.h"
#include "ui.h"

#define IFEEL_SETPOINT_DEFAULT 27
#define IFEEL_SETPOINT_MIN 24
#define IFEEL_SETPOINT_MAX 28
#define IFEEL_TEMP_HIGH 25.0f          /* decrease setpoint when room > this */
#define IFEEL_TEMP_LOW 23.5f           /* increase setpoint when room < this */
#define IFEEL_MONITOR_INTERVAL_S 300LL /* seconds between adjustments (5 min) */

static const char *TAG = "ifeel";

static ifeel_state_t s_state = IFEEL_OFF;
static uint8_t s_setpoint = IFEEL_SETPOINT_DEFAULT;
static bool s_light = false;
static int64_t s_last_monitor_us = 0;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

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

static void enter_on(void)
{
    s_state = IFEEL_ON;
    s_setpoint = IFEEL_SETPOINT_DEFAULT;
    s_last_monitor_us = esp_timer_get_time();
    led_on_for(3);
    ac_turn_on();
    ui_set_led_indicator(true);
    ui_set_bar(0, 0, IFEEL_MONITOR_INTERVAL_S);
    char buf[16];
    snprintf(buf, sizeof(buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
    ui_set_top_label(buf);
    ESP_LOGI(TAG, "State → ON");
}

static void enter_off(void)
{
    s_state = IFEEL_OFF;
    led_on_for(1);
    ac_turn_off();
    ui_set_led_indicator(false);
    ui_set_bar(0, 0, IFEEL_MONITOR_INTERVAL_S);
    ui_set_top_label("Gree iFeel");
    ESP_LOGI(TAG, "State → OFF");
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t ifeel_init(void)
{
    s_state = IFEEL_OFF;
    s_setpoint = IFEEL_SETPOINT_DEFAULT;
    s_last_monitor_us = 0;
    ESP_LOGI(TAG, "State → OFF (AC untouched)");
    return ESP_OK;
}

void ifeel_button_pressed(void)
{
    switch (s_state) {
    case IFEEL_OFF:
        enter_on();
        break;
    case IFEEL_ON:
        enter_off();
        break;
    }
}

void ifeel_on_temperature(float temperature)
{
    /* Always update the room temperature label */
    char rt_buf[16];
    snprintf(rt_buf, sizeof(rt_buf), "RT: %.1f\xC2\xB0\x43", temperature);
    ui_set_mid_label(rt_buf);

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

    if (temperature > IFEEL_TEMP_HIGH && s_setpoint > IFEEL_SETPOINT_MIN) {
        new_setpoint = s_setpoint - 1;
    } else if (temperature < IFEEL_TEMP_LOW && s_setpoint < IFEEL_SETPOINT_MAX) {
        new_setpoint = s_setpoint + 1;
    }

    if (new_setpoint != s_setpoint) {
        s_setpoint = new_setpoint;
        ESP_LOGI(TAG, "Adjust setpoint → %d°C (room=%.1f°C)", s_setpoint, temperature);
        char st_buf[16];
        snprintf(st_buf, sizeof(st_buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
        ui_set_top_label(st_buf);
        ac_turn_on();
    } else {
        ESP_LOGI(TAG, "No adjustment (room=%.1f°C setpoint=%d°C)", temperature, s_setpoint);
    }
}

void ifeel_temperature_pressed(void)
{
    if (s_state != IFEEL_ON) {
        return;
    }
    s_setpoint = (s_setpoint >= IFEEL_SETPOINT_MAX) ? IFEEL_SETPOINT_MIN : s_setpoint + 1;
    ESP_LOGI(TAG, "Temperature button: setpoint → %d°C", s_setpoint);
    char buf[16];
    snprintf(buf, sizeof(buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
    ui_set_top_label(buf);
    ac_turn_on();
}

void ifeel_light_pressed(void)
{
    s_light = !s_light;
    ESP_LOGI(TAG, "Light button: light → %d", s_light);
    gree_ac_state_t ac = {
        .power = (s_state == IFEEL_ON),
        .mode = GREE_MODE_COOL,
        .temperature = s_setpoint,
        .fan = GREE_FAN_AUTO,
        .light = s_light,
    };
    gree_ir_send(&ac);
}

ifeel_state_t ifeel_get_state(void) { return s_state; }

uint8_t ifeel_get_setpoint(void) { return s_setpoint; }
