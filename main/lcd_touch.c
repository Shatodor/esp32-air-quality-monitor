#include "lcd_touch.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// CH422G addresses and commands
#define CH422G_ADDR_IO       0x24   // I/O expander configuration register
#define CH422G_ADDR_CMD      0x38   // Backlight control register
#define CH422G_CMD_OUTPUT_EN 0x01   // Set to output mode
#define CH422G_CMD_BL_ON     0x1E   // Backlight on
#define CH422G_CMD_BL_OFF    0x1A   // Backlight off
#define CH422G_CMD_RST_1     0x2C   // Reset 1
#define CH422G_CMD_RST_2     0x2E   // Reset 2

static const char *TAG = "TOUCH";

static i2c_master_dev_handle_t dev_handle_io = NULL;   // for address 0x24
static i2c_master_dev_handle_t dev_handle_cmd = NULL;   // for address 0x38


void lcd_gpio_init(void) {
    ESP_LOGI(TAG, "Initialize GPIO");
    gpio_config_t gpio_conf = {};                    // Zero-initialize the config structure
    gpio_conf.intr_type = GPIO_INTR_DISABLE;         // Disable interrupt
    gpio_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;     // Bit mask of the pins, use GPIO4 here
    gpio_conf.mode = GPIO_MODE_OUTPUT;               // Set as input mode
    gpio_config(&gpio_conf);
}

esp_err_t lcd_io_expander_init(i2c_master_bus_handle_t *i2c_bus_handle) {
    ESP_LOGI(TAG, "Initialize expander");
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CH422G_ADDR_IO,
        .scl_speed_hz = I2C_EXPANDER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(*i2c_bus_handle, &dev_cfg, &dev_handle_io);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Added IO dev to I2C");
    else return ret;

    dev_cfg.device_address = CH422G_ADDR_CMD;
    ret = i2c_master_bus_add_device(*i2c_bus_handle, &dev_cfg, &dev_handle_cmd);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Added CMD dev to I2C");
    else return ret;

    return ESP_OK;
}

esp_err_t lcd_touch_reset(void) {
    if (dev_handle_io == NULL || dev_handle_cmd == NULL) {
        return ESP_ERR_INVALID_STATE;   // I2C not initialized
    }

    // Configure CH422G to output mode 
    uint8_t write_buf = CH422G_CMD_OUTPUT_EN;
    esp_err_t ret = i2c_master_transmit(dev_handle_io, &write_buf, 1, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret == ESP_OK) ESP_LOGI(TAG, "CH422G set to output");
    else return ret;

    // Reset the touch screen. 
    write_buf = CH422G_CMD_RST_1;
    ret = i2c_master_transmit(dev_handle_cmd, &write_buf, 1, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret == ESP_OK) ESP_LOGI(TAG, "Reset CMD 1 send");
    else return ret;

    esp_rom_delay_us(100 * 1000);
    gpio_set_level(GPIO_INPUT_IO_4, 0);
    esp_rom_delay_us(100 * 1000);

    write_buf = CH422G_CMD_RST_2;
    ret = i2c_master_transmit(dev_handle_cmd, &write_buf, 1, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret == ESP_OK) ESP_LOGI(TAG, "Reset CMD 2 send");
    else return ret;

    return ESP_OK; 
}

esp_err_t lcd_touch_init(i2c_master_bus_handle_t *i2c_bus_handle, esp_lcd_touch_handle_t *touch_handle) {
    ESP_LOGI(TAG, "Initialize touch IO (I2C)");  
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.scl_speed_hz = I2C_TOUCH_FREQ_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(*i2c_bus_handle, &io_config, &io_handle));
    ESP_LOGI(TAG, "Touch IO initialized");

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,        // No reset pin needed
        .int_gpio_num = GPIO_NUM_NC,        // No interrupt pin (using polling)
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Create the GT911 touch driver"); 
    esp_err_t ret =  esp_lcd_touch_new_i2c_gt911(io_handle, &touch_cfg, touch_handle);
    if (ret == ESP_OK) ESP_LOGI(TAG, "GT911 touch controller initialized");
    else ESP_LOGE(TAG, "GT911 touch controller initialize: FAIL");
    
    return ret;
}

esp_err_t lcd_touch_set_backlight(bool state) {
    if (dev_handle_io == NULL || dev_handle_cmd == NULL) {
        return ESP_ERR_INVALID_STATE;   // I2C not initialized
    }

    // Configure CH422G to output mode 
    uint8_t write_buf = CH422G_CMD_OUTPUT_EN;
    esp_err_t ret = i2c_master_transmit(dev_handle_io, &write_buf, 1, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret == ESP_OK) ESP_LOGI(TAG, "CH422G set to output");
    else return ret;

    //Send backlight command
    write_buf = state ? CH422G_CMD_BL_ON : CH422G_CMD_BL_OFF;
    ret = i2c_master_transmit(dev_handle_cmd, &write_buf, 1, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret == ESP_OK) ESP_LOGI(TAG, "Backlight set:  %s", state ? "ON" : "OFF");
    else return ret;

    return ESP_OK; 
}

