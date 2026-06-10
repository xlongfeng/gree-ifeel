/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "ui.h"

#include <stdio.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
static SemaphoreHandle_t s_lvgl_mutex;

#define UI_BAR_H 8

/* Main window widgets (visible when OFF) */
static lv_obj_t *s_main_win = NULL;
static lv_obj_t *s_main_rt_label = NULL;

/* Monitor window widgets (visible when ON) */
static lv_obj_t *s_monitor_win = NULL;
static lv_obj_t *s_monitor_st_label = NULL;
static lv_obj_t *s_monitor_rt_label = NULL;
static lv_obj_t *s_bar = NULL;

/* Message dialog widgets (brought to front for transient notifications) */
static lv_obj_t *s_msg_win = NULL;
static lv_obj_t *s_msg_label = NULL;
static lv_obj_t *s_limit_win = NULL;
static lv_obj_t *s_ht_label = NULL;
static lv_obj_t *s_lt_label = NULL;

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
        xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
        time_till_next_ms = lv_timer_handler();
        xSemaphoreGiveRecursive(s_lvgl_mutex);
        time_till_next_ms = MAX(time_till_next_ms, SSD1306_LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, SSD1306_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

/* ── Widget helpers ──────────────────────────────────────────────────────── */

static void win_style(lv_obj_t *win)
{
    lv_obj_set_size(win, SSD1306_LCD_H_RES, SSD1306_LCD_V_RES);
    lv_obj_set_pos(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_gap(win, 0, 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(win, lv_color_white(), 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    return lbl;
}

/* ── UI creation ─────────────────────────────────────────────────────────── */

static void lvgl_create_ui(lv_display_t *disp)
{
    /* Use the mono theme for correct monochrome rendering.
     * dark_bg=false: LVGL background=white → OLED OFF → physical dark screen,
     * foreground=black → OLED ON → physical bright elements (normal OLED look). */
    lv_theme_mono_init(disp, false, &lv_font_montserrat_12);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Limit window — created first, sits behind all other windows */
    s_limit_win = lv_obj_create(scr);
    win_style(s_limit_win);
    lv_obj_set_flex_flow(s_limit_win, LV_FLEX_FLOW_COLUMN);
    s_ht_label = make_label(s_limit_win, "HT: --.-\xC2\xB0\x43");
    s_lt_label = make_label(s_limit_win, "LT: --.-\xC2\xB0\x43");

    /* Monitor window — created second, behind main by default */
    s_monitor_win = lv_obj_create(scr);
    win_style(s_monitor_win);
    lv_obj_set_flex_flow(s_monitor_win, LV_FLEX_FLOW_COLUMN);
    s_monitor_st_label = make_label(s_monitor_win, "ST: --\xC2\xB0\x43");
    s_monitor_rt_label = make_label(s_monitor_win, "RT: --.-\xC2\xB0\x43");

    s_bar = lv_bar_create(s_monitor_win);
    lv_obj_set_size(s_bar, SSD1306_LCD_H_RES - 2, UI_BAR_H);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);

    /* Main window — created last so it starts in the foreground (OFF state) */
    s_main_win = lv_obj_create(scr);
    win_style(s_main_win);
    lv_obj_set_flex_flow(s_main_win, LV_FLEX_FLOW_COLUMN);
    make_label(s_main_win, "Gree iFeel");
    s_main_rt_label = make_label(s_main_win, "RT: --.-\xC2\xB0\x43");
    /* Message dialog — created last so it sits on top by default */
    s_msg_win = lv_obj_create(scr);
    win_style(s_msg_win);
    s_msg_label = make_label(s_msg_win, "");
    lv_obj_set_style_text_font(s_msg_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_msg_label, SSD1306_LCD_H_RES);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_background(s_msg_win); /* start hidden */
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
    // Brightness control (display still OFF at this point):
    //   0x81 = contrast (0x00=min … 0xFF=max, default 0x7F)
    //   0xD9 = pre-charge period (lower nibble = phase1, upper = phase2; default 0x22)
    //   0xDB = VCOMH deselect level (0x00/0x20/0x30/0x40; default 0x20)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x81, (uint8_t[]){0x20}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xD9, (uint8_t[]){0x11}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xDB, (uint8_t[]){0x00}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 28, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initialize LVGL");
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_lvgl_mutex);
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

    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    lvgl_create_ui(display);
    xSemaphoreGiveRecursive(s_lvgl_mutex);

    return ESP_OK;
}

void ui_show_limit(bool show)
{
    if (!s_limit_win)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    if (show) {
        lv_obj_move_foreground(s_limit_win);
    } else {
        lv_obj_move_background(s_limit_win);
    }
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_set_ht(const char *text)
{
    if (!s_ht_label)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    lv_label_set_text(s_ht_label, text);
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_set_lt(const char *text)
{
    if (!s_lt_label)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    lv_label_set_text(s_lt_label, text);
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_show_monitor(bool show)
{
    if (!s_monitor_win)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    if (show) {
        lv_obj_move_foreground(s_monitor_win);
    } else {
        lv_obj_move_background(s_monitor_win);
    }
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_set_st(const char *text)
{
    if (!s_monitor_st_label)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    lv_label_set_text(s_monitor_st_label, text);
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_set_rt(const char *text)
{
    if (!s_main_rt_label || !s_monitor_rt_label)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    lv_label_set_text(s_main_rt_label, text);
    lv_label_set_text(s_monitor_rt_label, text);
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_set_bar(int value, int min, int max)
{
    if (!s_bar)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    lv_bar_set_range(s_bar, min, max);
    lv_bar_set_value(s_bar, value, LV_ANIM_OFF);
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_show_msg(bool show)
{
    if (!s_msg_win)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    if (show) {
        lv_obj_move_foreground(s_msg_win);
    } else {
        lv_obj_move_background(s_msg_win);
    }
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_set_msg(const char *text)
{
    if (!s_msg_label)
        return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    lv_label_set_text(s_msg_label, text);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, 0);
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void ui_lock(void) { xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY); }

void ui_unlock(void) { xSemaphoreGiveRecursive(s_lvgl_mutex); }
