/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "ui.h"

#include <stdio.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "ui";

#define I2C_BUS_PORT 0

#define SSD1306_LCD_PIXEL_CLOCK_HZ (400 * 1000)
#define SSD1306_PIN_NUM_SDA 5
#define SSD1306_PIN_NUM_SCL 6
#define SSD1306_PIN_NUM_RST -1
#define SSD1306_I2C_HW_ADDR 0x3C

#define SSD1306_LCD_H_RES 72
#define SSD1306_LCD_V_RES 40
#define SSD1306_LCD_CMD_BITS 8
#define SSD1306_LCD_PARAM_BITS 8

#define SSD1306_LVGL_TICK_PERIOD_MS 5
#define SSD1306_LVGL_TASK_STACK_SIZE (4 * 1024)
#define SSD1306_LVGL_TASK_PRIORITY 2
#define SSD1306_LVGL_PALETTE_SIZE 8
#define SSD1306_LVGL_TASK_MAX_DELAY_MS 500
#define SSD1306_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ

static uint8_t oled_buffer[SSD1306_LCD_H_RES * SSD1306_LCD_V_RES / 8];
static _lock_t lvgl_api_lock;

/* ── Layout constants ────────────────────────────────────────────────────── */

#define UI_LABEL_MAX 8
#define UI_GAP 1 /* 1px gap between areas */

#define UI_TOP_H 12
#define UI_MID_H 16
#define UI_BOT_H 10
/* y positions: top=0, mid=13, bot=30  (12+1+16+1=30, total=40) */
#define UI_MID_Y (UI_TOP_H + UI_GAP)
#define UI_BOT_Y (UI_TOP_H + UI_GAP + UI_MID_H + UI_GAP)

#define UI_BAR_W 50
#define UI_LED_W (SSD1306_LCD_H_RES - UI_BAR_W) /* 22 */

#define UI_CYCLE_INTERVAL_US 2000000ULL /* 2 s */
#define UI_BLINK_INTERVAL_US 1000000ULL /* 1 s */

typedef struct {
    lv_obj_t *lv_label;
    bool hidden;
} ui_label_entry_t;

static ui_label_entry_t s_labels[UI_LABEL_MAX];
static int s_label_count = 0;
static int s_cycle_idx = 0;
static esp_timer_handle_t s_cycle_timer = NULL;

static lv_obj_t *s_led_indicator = NULL;
static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_mid_container = NULL;
static esp_timer_handle_t s_led_blink_timer = NULL;
static bool s_led_on = false;
static bool s_led_blink_state = false;

/* ── LVGL port callbacks ─────────────────────────────────────────────────── */

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io_panel, esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);

    // Skip the palette bytes reserved by LVGL for monochrome format
    px_map += SSD1306_LVGL_PALETTE_SIZE;

    uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);
    int x1 = area->x1;
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            bool chroma_color = (px_map[(hor_res >> 3) * y + (x >> 3)] & 1 << (7 - x % 8));
            uint8_t *buf = oled_buffer + hor_res * (y >> 3) + x;
            if (chroma_color) {
                (*buf) &= ~(1 << (y % 8));
            } else {
                (*buf) |= (1 << (y % 8));
            }
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, oled_buffer);
}

static void increase_lvgl_tick(void *arg) { lv_tick_inc(SSD1306_LVGL_TICK_PERIOD_MS); }

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        time_till_next_ms = MAX(time_till_next_ms, SSD1306_LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, SSD1306_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

/* ── Label cycle helpers ─────────────────────────────────────────────────── */

/* Advance to the next non-hidden label and update LVGL visibility. */
static void label_cycle_advance(void)
{
    if (s_label_count == 0)
        return;

    /* Find next non-hidden label starting after current index */
    int start = s_cycle_idx;
    int next = -1;
    for (int i = 1; i <= s_label_count; i++) {
        int idx = (start + i) % s_label_count;
        if (!s_labels[idx].hidden) {
            next = idx;
            break;
        }
    }
    /* Also accept current if nothing else is visible */
    if (next == -1 && !s_labels[start].hidden)
        next = start;
    if (next == -1) {
        /* All hidden: hide everything */
        for (int i = 0; i < s_label_count; i++)
            lv_obj_add_flag(s_labels[i].lv_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    s_cycle_idx = next;
    for (int i = 0; i < s_label_count; i++) {
        if (i == next) {
            lv_obj_remove_flag(s_labels[i].lv_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_labels[i].lv_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void label_cycle_cb(void *arg)
{
    ui_lock();
    label_cycle_advance();
    ui_unlock();
}

/* ── LED blink helper ────────────────────────────────────────────────────── */

static void led_blink_cb(void *arg)
{
    ui_lock();
    s_led_blink_state = !s_led_blink_state;
    lv_obj_set_style_bg_opa(s_led_indicator, s_led_blink_state ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    ui_unlock();
}

/* ── UI widgets ──────────────────────────────────────────────────────────── */

static void container_style(lv_obj_t *obj)
{
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(obj, 0, 0);
}

static void lvgl_create_ui(lv_display_t *disp)
{
    /* Use the mono theme for correct monochrome rendering.
     * dark_bg=false: LVGL background=white → OLED OFF → physical dark screen,
     * foreground=black → OLED ON → physical bright elements (normal OLED look). */
    lv_theme_mono_init(disp, false, &lv_font_montserrat_14);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Top area — fixed "GREE iFeel" label */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, SSD1306_LCD_H_RES, UI_TOP_H);
    container_style(top);
    lv_obj_t *top_label = lv_label_create(top);
    lv_label_set_text(top_label, "Gree iFeel");
    lv_label_set_long_mode(top_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(top_label, &lv_font_montserrat_12, 0);
    lv_obj_set_size(top_label, SSD1306_LCD_H_RES, UI_TOP_H);
    lv_obj_align(top_label, LV_ALIGN_TOP_MID, 0, 0);

    /* Mid area — label stack */
    lv_obj_t *mid = lv_obj_create(scr);
    s_mid_container = mid;
    lv_obj_set_pos(mid, 0, UI_MID_Y);
    lv_obj_set_size(mid, SSD1306_LCD_H_RES, UI_MID_H);
    container_style(mid);

    /* Attach any already-pushed labels */
    for (int i = 0; i < s_label_count; i++) {
        lv_obj_set_parent(s_labels[i].lv_label, mid);
        lv_obj_set_size(s_labels[i].lv_label, SSD1306_LCD_H_RES, UI_MID_H);
        lv_obj_align(s_labels[i].lv_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(s_labels[i].lv_label, LV_OBJ_FLAG_HIDDEN);
    }
    label_cycle_advance(); /* show first non-hidden label */

    /* Start 1s cycle timer */
    esp_timer_create_args_t cycle_args = {
        .callback = label_cycle_cb,
        .name = "ui_cycle",
    };
    esp_timer_create(&cycle_args, &s_cycle_timer);
    esp_timer_start_periodic(s_cycle_timer, UI_CYCLE_INTERVAL_US);

    /* Bottom area */
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_set_pos(bot, 0, UI_BOT_Y);
    lv_obj_set_size(bot, SSD1306_LCD_H_RES, UI_BOT_H);
    container_style(bot);

    /* Bar (left, 50px) */
    s_bar = lv_bar_create(bot);
    lv_obj_set_size(s_bar, UI_BAR_W - 2, UI_BOT_H - 2);
    lv_obj_align(s_bar, LV_ALIGN_LEFT_MID, 1, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    /* Explicit monochrome bar styles: transparent bg with bright border, bright indicator */
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, lv_color_black(), LV_PART_MAIN); /* black → OLED ON */
    lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), LV_PART_INDICATOR); /* black → OLED ON */
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);

    /* LED indicator (right, circular — 8×8 centred in the right portion) */
    s_led_indicator = lv_obj_create(bot);
    lv_obj_set_size(s_led_indicator, UI_BOT_H - 2, UI_BOT_H - 2); /* square for circle */
    lv_obj_align(s_led_indicator, LV_ALIGN_RIGHT_MID, -1, 0);
    lv_obj_set_style_pad_all(s_led_indicator, 0, 0);
    lv_obj_set_style_border_width(s_led_indicator, 1, 0);
    lv_obj_set_style_radius(s_led_indicator, LV_RADIUS_CIRCLE, 0);
    /* black → OLED ON (bright); initially solid (OFF state = static solid) */
    lv_obj_set_style_bg_color(s_led_indicator, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_led_indicator, LV_OPA_COVER, 0);

    /* Create LED blink timer (not started yet) */
    esp_timer_create_args_t blink_args = {
        .callback = led_blink_cb,
        .name = "ui_led_blink",
    };
    esp_timer_create(&blink_args, &s_led_blink_timer);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "Initialize I2C bus");
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = SSD1306_PIN_NUM_SDA,
        .scl_io_num = SSD1306_PIN_NUM_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = SSD1306_I2C_HW_ADDR,
        .scl_speed_hz = SSD1306_LCD_PIXEL_CLOCK_HZ,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = SSD1306_LCD_CMD_BITS,
        .lcd_param_bits = SSD1306_LCD_PARAM_BITS,
        .dc_bit_offset = 6,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install SSD1306 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = SSD1306_PIN_NUM_RST,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = SSD1306_LCD_V_RES,
    };
    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // The ESP-IDF driver sets COM pins config 0x02 (sequential) for height != 64,
    // but this display uses alternating COM pins (0x12), causing every other row to be
    // skipped and content appearing at 2x height. Override with the correct value.
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xDA, (uint8_t[]){0x12}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 28, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();
    lv_display_t *display = lv_display_create(SSD1306_LCD_H_RES, SSD1306_LCD_V_RES);
    lv_display_set_user_data(display, panel_handle);

    size_t draw_buffer_sz = SSD1306_LCD_H_RES * SSD1306_LCD_V_RES / 8 + SSD1306_LVGL_PALETTE_SIZE;
    void *buf = heap_caps_calloc(1, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);

    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display, buf, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

    ESP_LOGI(TAG, "Start LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, SSD1306_LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreate(lvgl_port_task, "LVGL", SSD1306_LVGL_TASK_STACK_SIZE, NULL, SSD1306_LVGL_TASK_PRIORITY, NULL);

    _lock_acquire(&lvgl_api_lock);
    lvgl_create_ui(display);
    _lock_release(&lvgl_api_lock);

    return ESP_OK;
}

void ui_lock(void) { _lock_acquire(&lvgl_api_lock); }

void ui_unlock(void) { _lock_release(&lvgl_api_lock); }

ui_label_id_t ui_label_push(const char *text)
{
    if (s_label_count >= UI_LABEL_MAX) {
        ESP_LOGE(TAG, "Label stack full");
        return -1;
    }
    ui_label_id_t id = s_label_count++;
    ui_label_entry_t *e = &s_labels[id];

    lv_obj_t *parent = s_mid_container ? s_mid_container : lv_screen_active();
    e->lv_label = lv_label_create(parent);
    lv_label_set_text(e->lv_label, text);
    lv_label_set_long_mode(e->lv_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(e->lv_label, &lv_font_montserrat_12, 0);
    lv_obj_set_size(e->lv_label, SSD1306_LCD_H_RES, UI_MID_H);
    lv_obj_align(e->lv_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(e->lv_label, LV_OBJ_FLAG_HIDDEN);
    e->hidden = false;

    return id;
}

void ui_label_show(ui_label_id_t id)
{
    if (id < 0 || id >= s_label_count)
        return;
    s_labels[id].hidden = false;
}

void ui_label_hide(ui_label_id_t id)
{
    if (id < 0 || id >= s_label_count)
        return;
    s_labels[id].hidden = true;
}

void ui_label_set_text(ui_label_id_t id, const char *text)
{
    if (id < 0 || id >= s_label_count)
        return;
    lv_label_set_text(s_labels[id].lv_label, text);
}

void ui_set_led_indicator(bool on)
{
    if (!s_led_indicator || !s_led_blink_timer)
        return;
    s_led_on = on;
    if (on) {
        /* ON: blink */
        s_led_blink_state = true;
        ui_lock();
        lv_obj_set_style_bg_opa(s_led_indicator, LV_OPA_COVER, 0);
        ui_unlock();
        esp_timer_stop(s_led_blink_timer);
        esp_timer_start_periodic(s_led_blink_timer, UI_BLINK_INTERVAL_US);
    } else {
        /* OFF: static solid */
        esp_timer_stop(s_led_blink_timer);
        s_led_blink_state = false;
        ui_lock();
        lv_obj_set_style_bg_opa(s_led_indicator, LV_OPA_COVER, 0);
        ui_unlock();
    }
}

void ui_set_bar(int value, int min, int max)
{
    if (!s_bar)
        return;
    ui_lock();
    lv_bar_set_range(s_bar, min, max);
    lv_bar_set_value(s_bar, value, LV_ANIM_OFF);
    ui_unlock();
}
