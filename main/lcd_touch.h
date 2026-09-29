#ifndef _LCD_TOUCH_H_
#define _RGB_TOUCH_H_

#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"

#define I2C_EXPANDER_FREQ_HZ    100000  //I2C master clock frequency 
#define I2C_TOUCH_FREQ_HZ       100000  // Standard for GT911
#define I2C_MASTER_TIMEOUT_MS   1000

#define GPIO_INPUT_IO_4         4
#define GPIO_INPUT_PIN_SEL      1ULL<<GPIO_INPUT_IO_4

#define LCD_H_RES               (800)
#define LCD_V_RES               (400)

void lcd_gpio_init(void);
esp_err_t lcd_touch_reset(void);
esp_err_t lcd_io_expander_init(i2c_master_bus_handle_t *i2c_bus_handle);
esp_err_t lcd_touch_init(i2c_master_bus_handle_t *i2c_bus_handle, esp_lcd_touch_handle_t *touch_handle);
esp_err_t lcd_touch_set_backlight(bool state);

#endif