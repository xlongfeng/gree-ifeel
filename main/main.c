#include "button.h"
#include "console.h"
#include "gree_ir.h"
#include "ifeel.h"
#include "thermometer.h"
#include "ui.h"

void app_main(void)
{
    ESP_ERROR_CHECK(console_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(gree_ir_init(CONFIG_IFEEL_IR_GPIO));
    ESP_ERROR_CHECK(button_init(CONFIG_IFEEL_BUTTON_0_GPIO, LV_KEY_BUTTON_0, LV_KEY_ALT_BUTTON_0));
    ESP_ERROR_CHECK(button_init(CONFIG_IFEEL_BUTTON_1_GPIO, LV_KEY_BUTTON_1, LV_KEY_ALT_BUTTON_1));
    ESP_ERROR_CHECK(button_init(CONFIG_IFEEL_BUTTON_2_GPIO, LV_KEY_BUTTON_2, LV_KEY_ALT_BUTTON_2));
    button_indev_create();
    ESP_ERROR_CHECK(ifeel_init());
    ESP_ERROR_CHECK(thermometer_init(CONFIG_IFEEL_DS18B20_GPIO, ifeel_on_temperature));
}
