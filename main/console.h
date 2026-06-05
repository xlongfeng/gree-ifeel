#pragma once

#include "esp_err.h"

/**
 * Install USB Serial JTAG driver with ring-buffer support and start a task
 * that continuously drains the RX FIFO. Without this, idf-monitor write
 * operations time out when the device's RX buffer is full.
 */
esp_err_t console_init(void);
