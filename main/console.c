#include "console.h"

#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "console";

static void console_rx_task(void *arg)
{
    uint8_t buf[64];
    while (1) {
        usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
    }
}

esp_err_t console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG, "driver install failed");
    xTaskCreate(console_rx_task, "console_rx", 1024, NULL, 1, NULL);
    return ESP_OK;
}
