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
#include "gree_ir.h"
#include "lvgl.h"
#include "ui.h"

#define IFEEL_SETPOINT_DEFAULT 24
#define IFEEL_SETPOINT_MIN 24
#define IFEEL_SETPOINT_MAX 28
#define IFEEL_MONITOR_INTERVAL_S 300LL /* seconds between adjustments (5 min) */

/* Temp limit config */
#define LIMIT_STEPS 6
#define LIMIT_INDEX_DEFAULT 3
#define LIMIT_LOW_BASE 23.4f  /* LOW  at index 0 */
#define LIMIT_HIGH_BASE 25.0f /* HIGH at index 0 */
#define LIMIT_STRIDE 0.2f
#define LIMIT_AUTO_HIDE_MS 8000U /* 8 s */
#define MSG_AUTO_HIDE_MS 500U    /* 0.5 s */

static const char *TAG = "ifeel";

static ifeel_state_t s_state = IFEEL_OFF;
static uint8_t s_setpoint = IFEEL_SETPOINT_DEFAULT;
static bool s_light = false;
static int64_t s_last_monitor_us = 0;

static int s_limit_index = LIMIT_INDEX_DEFAULT;

/* LVGL groups — one per window state */
static lv_group_t *s_main_group = NULL;
static lv_group_t *s_monitor_group = NULL;
static lv_group_t *s_limit_group = NULL;
static lv_group_t *s_msg_group = NULL;

/* Invisible controller objects — focused members of each group */
static lv_obj_t *s_main_ctrl = NULL;
static lv_obj_t *s_monitor_ctrl = NULL;
static lv_obj_t *s_limit_ctrl = NULL;
static lv_obj_t *s_msg_ctrl = NULL;

/* Group stack — mirrors the old dispatch stack */
#define GROUP_STACK_MAX 4
static lv_group_t *s_group_stack[GROUP_STACK_MAX];
static int s_group_top = 0;

/* Guard flags to prevent double push/pop */
static bool s_limit_pushed = false;
static bool s_monitor_pushed = false;
static bool s_msg_pushed = false;

/* LVGL auto-hide timers (created once, pause/resumed as needed) */
static lv_timer_t *s_limit_timer = NULL;
static lv_timer_t *s_msg_timer = NULL;

static float limit_low(void) { return LIMIT_LOW_BASE + s_limit_index * LIMIT_STRIDE; }
static float limit_high(void) { return LIMIT_HIGH_BASE + s_limit_index * LIMIT_STRIDE; }

/* ── Group stack helpers ──────────────────────────────────────────────────── */

static void group_push(lv_group_t *g)
{
    assert(s_group_top < GROUP_STACK_MAX && "Group stack overflow");
    s_group_stack[s_group_top++] = g;
    lv_indev_set_group(button_get_indev(), g);
}

static void group_pop(void)
{
    assert(s_group_top > 0 && "Group stack underflow");
    s_group_top--;
    lv_indev_set_group(button_get_indev(), s_group_top > 0 ? s_group_stack[s_group_top - 1] : NULL);
}

/* ── Limit window helpers ─────────────────────────────────────────────────── */

static void limit_update_ui(void)
{
    char ht[16], lt[16];
    snprintf(ht, sizeof(ht), "HT: %.1f\xC2\xB0\x43", limit_high());
    snprintf(lt, sizeof(lt), "LT: %.1f\xC2\xB0\x43", limit_low());
    ui_set_ht(ht);
    ui_set_lt(lt);
}

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

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void limit_show(void);
static void limit_hide(void);
static void msg_show(const char *text);
static void msg_hide(void);

/* ── Event callbacks ──────────────────────────────────────────────────────── */

static void enter_on(void);
static void enter_off(void);

static void cb_main(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_BUTTON_0) {
        enter_on();
        msg_show("Power ON");
    } else if (key == LV_KEY_ALT_BUTTON_1) {
        limit_show();
    } else if (key == LV_KEY_BUTTON_2) {
        /* toggle light */
        s_light = !s_light;
        gree_ac_state_t ac = {
            .power = false,
            .mode = GREE_MODE_COOL,
            .temperature = s_setpoint,
            .fan = GREE_FAN_AUTO,
            .light = s_light,
        };
        gree_ir_send(&ac);
        char text[16];
        snprintf(text, sizeof(text), "Light %s", s_light ? "ON" : "OFF");
        msg_show(text);
        ESP_LOGI(TAG, "F3: light → %d", s_light);
    }
}

static void cb_monitor(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_BUTTON_0) {
        enter_off();
        msg_show("Power OFF");
    } else if (key == LV_KEY_BUTTON_1) {
        s_setpoint = (s_setpoint >= IFEEL_SETPOINT_MAX) ? IFEEL_SETPOINT_MIN : s_setpoint + 1;
        ESP_LOGI(TAG, "F2: setpoint → %d°C", s_setpoint);
        char buf[16];
        snprintf(buf, sizeof(buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
        ui_set_st(buf);
        ac_turn_on();
    } else if (key == LV_KEY_ALT_BUTTON_1) {
        limit_show();
    } else if (key == LV_KEY_BUTTON_2) {
        s_light = !s_light;
        gree_ac_state_t ac = {
            .power = true,
            .mode = GREE_MODE_COOL,
            .temperature = s_setpoint,
            .fan = GREE_FAN_AUTO,
            .light = s_light,
        };
        gree_ir_send(&ac);
        char text[16];
        snprintf(text, sizeof(text), "Light %s", s_light ? "ON" : "OFF");
        msg_show(text);
        ESP_LOGI(TAG, "F3: light → %d", s_light);
    }
}

static void limit_restart_timer(void);

static void cb_limit(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_BUTTON_1) {
        s_limit_index = (s_limit_index + 1) % LIMIT_STEPS;
        limit_update_ui();
        limit_restart_timer();
        ESP_LOGI(TAG, "Limit cycle → index=%d LOW=%.1f HIGH=%.1f", s_limit_index, limit_low(), limit_high());
    } else {
        limit_hide();
    }
}

/* Message dialog handler — all keys ignored */
static void cb_msg(lv_event_t *e) { (void)e; }

/* ── Window transitions ───────────────────────────────────────────────────── */

static void enter_on(void)
{
    if (s_monitor_pushed)
        return;
    s_monitor_pushed = true;
    s_state = IFEEL_ON;
    s_setpoint = IFEEL_SETPOINT_DEFAULT;
    s_last_monitor_us = esp_timer_get_time();
    ac_turn_on();
    ui_set_bar(0, 0, IFEEL_MONITOR_INTERVAL_S);
    char buf[16];
    snprintf(buf, sizeof(buf), "ST: %d.0\xC2\xB0\x43", s_setpoint);
    ui_set_st(buf);
    ui_show_monitor(true);
    group_push(s_monitor_group);
    ESP_LOGI(TAG, "State → ON");
}

static void enter_off(void)
{
    if (!s_monitor_pushed)
        return;
    s_monitor_pushed = false;
    s_state = IFEEL_OFF;
    ac_turn_off();
    ui_set_bar(0, 0, IFEEL_MONITOR_INTERVAL_S);
    ui_show_monitor(false);
    group_pop();
    ESP_LOGI(TAG, "State → OFF");
}

/* ── Limit window ─────────────────────────────────────────────────────────── */

static void limit_timer_cb(lv_timer_t *timer);

static void limit_restart_timer(void)
{
    lv_timer_set_repeat_count(s_limit_timer, 1);
    lv_timer_reset(s_limit_timer);
    lv_timer_resume(s_limit_timer);
}

static void limit_show(void)
{
    if (s_limit_pushed)
        return;
    s_limit_pushed = true;
    limit_update_ui();
    ui_show_limit(true);
    limit_restart_timer();
    group_push(s_limit_group);
    ESP_LOGI(TAG, "Limit window shown (LOW=%.1f HIGH=%.1f)", limit_low(), limit_high());
}

static void limit_hide(void)
{
    if (!s_limit_pushed)
        return;
    s_limit_pushed = false;
    lv_timer_pause(s_limit_timer);
    ui_show_limit(false);
    group_pop();
    ESP_LOGI(TAG, "Limit window hidden");
}

static void limit_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* s_limit_timer is auto-paused after repeat_count reaches 0 */
    limit_hide();
    ESP_LOGI(TAG, "Limit window auto-hidden");
}

/* ── Message dialog ───────────────────────────────────────────────────────── */

static void msg_timer_cb(lv_timer_t *timer);

static void msg_show(const char *text)
{
    ui_set_msg(text);
    lv_timer_set_repeat_count(s_msg_timer, 1);
    lv_timer_reset(s_msg_timer);
    lv_timer_resume(s_msg_timer);
    if (!s_msg_pushed) {
        s_msg_pushed = true;
        ui_show_msg(true);
        group_push(s_msg_group);
    }
    ESP_LOGI(TAG, "Msg: %s", text);
}

static void msg_hide(void)
{
    if (!s_msg_pushed)
        return;
    s_msg_pushed = false;
    lv_timer_pause(s_msg_timer);
    ui_show_msg(false);
    group_pop();
    ESP_LOGI(TAG, "Msg hidden");
}

static void msg_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    msg_hide();
}

/* ── Public API ───────────────────────────────────────────────────────────── */

static lv_obj_t *make_ctrl(lv_group_t *group, lv_event_cb_t cb)
{
    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    lv_obj_set_size(obj, 0, 0);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_KEY, NULL);
    return obj;
}

esp_err_t ifeel_init(void)
{
    s_state = IFEEL_OFF;
    s_setpoint = IFEEL_SETPOINT_DEFAULT;
    s_last_monitor_us = 0;

    ui_lock();

    /* Create one group + invisible controller per window state */
    s_main_group = lv_group_create();
    s_monitor_group = lv_group_create();
    s_limit_group = lv_group_create();
    s_msg_group = lv_group_create();

    s_main_ctrl = make_ctrl(s_main_group, cb_main);
    s_monitor_ctrl = make_ctrl(s_monitor_group, cb_monitor);
    s_limit_ctrl = make_ctrl(s_limit_group, cb_limit);
    s_msg_ctrl = make_ctrl(s_msg_group, cb_msg);

    /* Create auto-hide timers (paused, auto_delete=false so we can reuse them) */
    s_limit_timer = lv_timer_create(limit_timer_cb, LIMIT_AUTO_HIDE_MS, NULL);
    lv_timer_set_auto_delete(s_limit_timer, false);
    lv_timer_pause(s_limit_timer);

    s_msg_timer = lv_timer_create(msg_timer_cb, MSG_AUTO_HIDE_MS, NULL);
    lv_timer_set_auto_delete(s_msg_timer, false);
    lv_timer_pause(s_msg_timer);

    /* Activate main (OFF) group */
    group_push(s_main_group);

    ui_unlock();

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
