/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>

#include "lvgl.h"

static lv_obj_t *s_temp_label = NULL;

void lvgl_create_ui(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    s_temp_label = lv_label_create(scr);
    lv_label_set_long_mode(s_temp_label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(s_temp_label, "Thermostatic Control");
    /* Size of the screen (if you use rotation 90 or 270, please use
     * lv_display_get_vertical_resolution) */
    lv_obj_set_width(s_temp_label, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(s_temp_label, LV_ALIGN_TOP_MID, 0, 0);
}

void lvgl_set_temperature(float temperature)
{
    if (s_temp_label == NULL) {
        return;
    }
    char text[64];
    sprintf(text, "RT: %.1f C", temperature);
    lv_label_set_text(s_temp_label, text);
}
