/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>

#include "lvgl.h"

void lvgl_update_ui(lv_display_t *disp, uint16_t rt)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_t *label = lv_label_create(scr);
    char text[64];

    sprintf(text, "RT: %d.%d °C", rt / 10, rt % 10);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(label, text);
    /* Size of the screen (if you use rotation 90 or 270, please use
     * lv_display_get_vertical_resolution) */
    lv_obj_set_width(label, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
}
