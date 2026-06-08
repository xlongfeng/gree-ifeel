#include "button.h"
#include "console.h"
#include "driver/gpio.h"
#include "gree_ir.h"
#include "ifeel.h"
#include "thermometer.h"
#include "ui.h"

#define DS18B20_GPIO_NUM CONFIG_IFEEL_DS18B20_GPIO
#define F1_BUTTON_GPIO_NUM CONFIG_IFEEL_F1_BUTTON_GPIO
#define F2_BUTTON_GPIO_NUM CONFIG_IFEEL_F2_BUTTON_GPIO
#define F3_BUTTON_GPIO_NUM CONFIG_IFEEL_F3_BUTTON_GPIO
#define GREE_IR_GPIO_NUM CONFIG_IFEEL_IR_GPIO

void app_main(void)
{
    ESP_ERROR_CHECK(console_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(gree_ir_init(GREE_IR_GPIO_NUM));
    ESP_ERROR_CHECK(ifeel_init());
    ESP_ERROR_CHECK(thermometer_init(DS18B20_GPIO_NUM, ifeel_on_temperature));
    ESP_ERROR_CHECK(button_init(F1_BUTTON_GPIO_NUM));
    ESP_ERROR_CHECK(button_init(F2_BUTTON_GPIO_NUM));
    ESP_ERROR_CHECK(button_init(F3_BUTTON_GPIO_NUM));
}
