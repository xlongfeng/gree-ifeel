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

#define UI_GAP 1 /* 1px gap between areas */

#define UI_TOP_H 12
#define UI_MID_H 16
#define UI_BOT_H 10
/* y positions: top=0, mid=13, bot=30  (12+1+16+1=30, total=40) */
#define UI_MID_Y (UI_TOP_H + UI_GAP)
#define UI_BOT_Y (UI_TOP_H + UI_GAP + UI_MID_H + UI_GAP)

#define UI_BLINK_INTERVAL_US 1000000ULL /* 1 s */

static lv_obj_t *s_top_label = NULL;
static lv_obj_t *s_mid_label = NULL;

static lv_obj_t *s_bar = NULL;
static esp_timer_handle_t s_bar_blink_timer = NULL;
static bool s_bar_blink_state = false;

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
/* ── Bar blink helper ────────────────────────────────────────────────────── */

static void bar_blink_cb(void *arg)
{
    _lock_acquire(&lvgl_api_lock);
    s_bar_blink_state = !s_bar_blink_state;
    if (s_bar_blink_state) {
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
    _lock_release(&lvgl_api_lock);
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
    lv_theme_mono_init(disp, false, &lv_font_montserrat_12);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Top area — label: "Gree iFeel" (OFF) or setpoint (ON) */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, SSD1306_LCD_H_RES, UI_TOP_H);
    container_style(top);
    s_top_label = lv_label_create(top);
    lv_label_set_text(s_top_label, "Gree iFeel");
    lv_label_set_long_mode(s_top_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_top_label, &lv_font_montserrat_12, 0);
    lv_obj_set_size(s_top_label, SSD1306_LCD_H_RES, UI_TOP_H);
    lv_obj_align(s_top_label, LV_ALIGN_TOP_MID, 0, 0);

    /* Mid area — single label for room temperature */
    lv_obj_t *mid = lv_obj_create(scr);
    lv_obj_set_pos(mid, 0, UI_MID_Y);
    lv_obj_set_size(mid, SSD1306_LCD_H_RES, UI_MID_H);
    container_style(mid);
    s_mid_label = lv_label_create(mid);
    lv_label_set_text(s_mid_label, "RT: --.-\xC2\xB0\x43");
    lv_label_set_long_mode(s_mid_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_mid_label, &lv_font_montserrat_12, 0);
    lv_obj_set_size(s_mid_label, SSD1306_LCD_H_RES, UI_MID_H);
    lv_obj_align(s_mid_label, LV_ALIGN_CENTER, 0, 0);

    /* Bottom area */
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_set_pos(bot, 0, UI_BOT_Y);
    lv_obj_set_size(bot, SSD1306_LCD_H_RES, UI_BOT_H);
    container_style(bot);

    /* Bar — full width */
    s_bar = lv_bar_create(bot);
    lv_obj_set_size(s_bar, SSD1306_LCD_H_RES - 2, UI_BOT_H - 2);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    /* Explicit monochrome bar styles: transparent bg with bright border, bright indicator */
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);

    /* Bar blink timer — starts immediately (OFF state = blinking) */
    esp_timer_create_args_t blink_args = {
        .callback = bar_blink_cb,
        .name = "ui_bar_blink",
    };
    esp_timer_create(&blink_args, &s_bar_blink_timer);
    s_bar_blink_state = true;
    esp_timer_start_periodic(s_bar_blink_timer, UI_BLINK_INTERVAL_US);
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

void ui_set_top_label(const char *text)
{
    if (!s_top_label)
        return;
    _lock_acquire(&lvgl_api_lock);
    lv_label_set_text(s_top_label, text);
    _lock_release(&lvgl_api_lock);
}

void ui_set_mid_label(const char *text)
{
    if (!s_mid_label)
        return;
    _lock_acquire(&lvgl_api_lock);
    lv_label_set_text(s_mid_label, text);
    _lock_release(&lvgl_api_lock);
}

void ui_set_bar_blinking(bool blink)
{
    if (!s_bar || !s_bar_blink_timer)
        return;
    if (blink) {
        s_bar_blink_state = true;
        esp_timer_stop(s_bar_blink_timer);
        esp_timer_start_periodic(s_bar_blink_timer, UI_BLINK_INTERVAL_US);
    } else {
        esp_timer_stop(s_bar_blink_timer);
        s_bar_blink_state = false;
        /* Ensure bar is visible when blinking stops */
        _lock_acquire(&lvgl_api_lock);
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        _lock_release(&lvgl_api_lock);
    }
}

void ui_set_bar(int value, int min, int max)
{
    if (!s_bar)
        return;
    _lock_acquire(&lvgl_api_lock);
    lv_bar_set_range(s_bar, min, max);
    lv_bar_set_value(s_bar, value, LV_ANIM_OFF);
    _lock_release(&lvgl_api_lock);
}
