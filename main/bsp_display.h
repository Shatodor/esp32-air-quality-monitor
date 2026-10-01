#ifndef BSP_DISPLAY_H
#define BSP_DISPLAY_H

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * Board support for the Waveshare ESP32-S3-Touch-LCD-4.3 display stack:
 *   - CH422G I/O expander (touch reset + backlight control)
 *   - GT911 capacitive touch over I2C
 *   - 800x480 RGB565 LCD panel
 *   - LVGL port binding (display + touch)
 *
 * Call once, after the I2C master bus is created.
 * Must be called from a single task before any LVGL call.
 */
esp_err_t bsp_display_init(i2c_master_bus_handle_t *i2c_bus);

/* Turn the LCD backlight on or off. */
esp_err_t bsp_display_set_backlight(bool on);

#endif // BSP_DISPLAY_H