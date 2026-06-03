/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "ds18b20.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gree_ir.h"
#include "lvgl.h"
#include "onewire_bus.h"

#include "esp_lcd_panel_vendor.h"

static const char *TAG = "thermostatic";

#define DS18B20_GPIO_NUM GPIO_NUM_0
#define GREE_IR_GPIO_NUM GPIO_NUM_4

#define I2C_BUS_PORT 0

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your
/// LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define SSD1306_LCD_PIXEL_CLOCK_HZ (400 * 1000)
#define SSD1306_PIN_NUM_SDA 5
#define SSD1306_PIN_NUM_SCL 6
#define SSD1306_PIN_NUM_RST -1
#define SSD1306_I2C_HW_ADDR 0x3C

// The pixel number in horizontal and vertical
#define SSD1306_LCD_H_RES 72
#define SSD1306_LCD_V_RES 40

// Bit number used to represent command and parameter
#define SSD1306_LCD_CMD_BITS 8
#define SSD1306_LCD_PARAM_BITS 8

#define SSD1306_LVGL_TICK_PERIOD_MS 5
#define SSD1306_LVGL_TASK_STACK_SIZE (4 * 1024)
#define SSD1306_LVGL_TASK_PRIORITY 2
#define SSD1306_LVGL_PALETTE_SIZE 8
#define SSD1306_LVGL_TASK_MAX_DELAY_MS 500
#define SSD1306_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ

// To use LV_COLOR_FORMAT_I1, we need an extra buffer to hold the converted data
static uint8_t oled_buffer[SSD1306_LCD_H_RES * SSD1306_LCD_V_RES / 8];

// LVGL library is not thread-safe, use a mutex to protect it
static _lock_t lvgl_api_lock;

extern void lvgl_create_ui(lv_disp_t *disp);
extern void lvgl_set_temperature(float temperature);

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

    // This is necessary because LVGL reserves 2 x 4 bytes in the buffer, as these
    // are assumed to be used as a palette. Skip the palette here More information
    // about the monochrome, please refer to
    // https://docs.lvgl.io/9.2/porting/display.html#monochrome-displays
    px_map += SSD1306_LVGL_PALETTE_SIZE;

    uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);
    int x1 = area->x1;
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            /* The order of bits is MSB first
                        MSB           LSB
               bits      7 6 5 4 3 2 1 0
               pixels    0 1 2 3 4 5 6 7
                        Left         Right
            */
            bool chroma_color = (px_map[(hor_res >> 3) * y + (x >> 3)] & 1 << (7 - x % 8));

            /* Write to the buffer as required for the display.
             * It writes only 1-bit for monochrome displays mapped vertically.*/
            uint8_t *buf = oled_buffer + hor_res * (y >> 3) + (x);
            if (chroma_color) {
                (*buf) &= ~(1 << (y % 8));
            } else {
                (*buf) |= (1 << (y % 8));
            }
        }
    }
    // pass the draw buffer to the driver
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, oled_buffer);
}

static void increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(SSD1306_LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        // in case of triggering a task watch dog time out
        time_till_next_ms = MAX(time_till_next_ms, SSD1306_LVGL_TASK_MIN_DELAY_MS);
        // in case of lvgl display not ready yet
        time_till_next_ms = MIN(time_till_next_ms, SSD1306_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

static void temperature_task(void *arg)
{
    ds18b20_device_handle_t s_ds18b20 = NULL;

    ESP_LOGI(TAG, "Initialize DS18B20 on GPIO%d", DS18B20_GPIO_NUM);
    onewire_bus_handle_t onewire_bus = NULL;
    onewire_bus_config_t onewire_bus_config = {
        .bus_gpio_num = DS18B20_GPIO_NUM,
    };
    onewire_bus_rmt_config_t onewire_rmt_config = {
        .max_rx_bytes = 10,
    };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&onewire_bus_config, &onewire_rmt_config, &onewire_bus));

    ds18b20_config_t ds_cfg = {};
    if (ds18b20_new_device_from_bus(onewire_bus, &ds_cfg, &s_ds18b20) == ESP_OK) {
        ESP_LOGI(TAG, "DS18B20 found on GPIO%d", DS18B20_GPIO_NUM);
    } else {
        ESP_LOGW(TAG, "No DS18B20 device found on GPIO%d", DS18B20_GPIO_NUM);
    }

    while (1) {
        if (s_ds18b20 != NULL) {
            float temperature = 0.0f;
            if (ds18b20_trigger_temperature_conversion(s_ds18b20) == ESP_OK &&
                ds18b20_get_temperature(s_ds18b20, &temperature) == ESP_OK) {
                ESP_LOGI(TAG, "Temperature: %.1f C", temperature);
                _lock_acquire(&lvgl_api_lock);
                lvgl_set_temperature(temperature);
                _lock_release(&lvgl_api_lock);
            } else {
                ESP_LOGW(TAG, "Failed to read DS18B20 temperature");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
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
        .control_phase_bytes = 1,               // According to SSD1306 datasheet
        .lcd_cmd_bits = SSD1306_LCD_CMD_BITS,   // According to SSD1306 datasheet
        .lcd_param_bits = SSD1306_LCD_CMD_BITS, // According to SSD1306 datasheet
        .dc_bit_offset = 6,                     // According to SSD1306 datasheet
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
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 28, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();
    // create a lvgl display
    lv_display_t *display = lv_display_create(SSD1306_LCD_H_RES, SSD1306_LCD_V_RES);
    // associate the i2c panel handle to the display
    lv_display_set_user_data(display, panel_handle);
    // create draw buffer
    void *buf = NULL;
    ESP_LOGI(TAG, "Allocate separate LVGL draw buffers");
    // LVGL reserves 2 x 4 bytes in the buffer, as these are assumed to be used as
    // a palette.
    size_t draw_buffer_sz = SSD1306_LCD_H_RES * SSD1306_LCD_V_RES / 8 + SSD1306_LVGL_PALETTE_SIZE;
    buf = heap_caps_calloc(1, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);

    // LVGL9 suooprt new monochromatic format.
    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    // initialize LVGL draw buffers
    lv_display_set_buffers(display, buf, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    // set the callback which can copy the rendered image to an area of the
    // display
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    /* Register done callback */
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display);

    ESP_LOGI(TAG, "Use esp_timer as LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {.callback = &increase_lvgl_tick, .name = "lvgl_tick"};
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, SSD1306_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(lvgl_port_task, "LVGL", SSD1306_LVGL_TASK_STACK_SIZE, NULL, SSD1306_LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Display LVGL Scroll Text");
    // Lock the mutex due to the LVGL APIs are not thread-safe
    _lock_acquire(&lvgl_api_lock);
    lvgl_create_ui(display);
    _lock_release(&lvgl_api_lock);

    xTaskCreate(temperature_task, "ds18b20", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(gree_ir_init(GREE_IR_GPIO_NUM));
}
