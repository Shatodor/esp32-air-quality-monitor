#include "bsp_display.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"

#define TAG "BSP_DISPLAY"

/* ============================================================
 * Board wiring
 * ============================================================ */

/* GT911 INT pin — used to select I2C address during reset */
#define LCD_TOUCH_INT_GPIO      4
#define LCD_TOUCH_INT_SEL       (1ULL << LCD_TOUCH_INT_GPIO)

/* I2C addresses of CH422G I/O expander */
#define CH422G_ADDR_IO          0x24   /* configuration register */
#define CH422G_ADDR_CMD         0x38   /* output register */

/* CH422G commands */
#define CH422G_CMD_OUT_EN       0x01
#define CH422G_CMD_BL_ON        0x1E
#define CH422G_CMD_BL_OFF       0x1A
#define CH422G_CMD_RST_1        0x2C
#define CH422G_CMD_RST_2        0x2E

#define I2C_EXPANDER_FREQ_HZ    100000
#define I2C_TOUCH_FREQ_HZ       100000
#define I2C_TIMEOUT_MS          1000

/* ============================================================
 * LCD panel
 * ============================================================ */

#define LCD_H_RES               800
#define LCD_V_RES               480
#define LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)
#define LCD_RGB_DATA_WIDTH      16
#define LCD_RGB_BITS_PER_PIXEL  16
#define LCD_NUM_FBS             2
#define LCD_BOUNCE_LINES        20   /* bounce buffer height in lines */

#define LCD_IO_VSYNC            GPIO_NUM_3
#define LCD_IO_HSYNC            GPIO_NUM_46
#define LCD_IO_DE               GPIO_NUM_5
#define LCD_IO_PCLK             GPIO_NUM_7
#define LCD_IO_DATA0            GPIO_NUM_14
#define LCD_IO_DATA1            GPIO_NUM_38
#define LCD_IO_DATA2            GPIO_NUM_18
#define LCD_IO_DATA3            GPIO_NUM_17
#define LCD_IO_DATA4            GPIO_NUM_10
#define LCD_IO_DATA5            GPIO_NUM_39
#define LCD_IO_DATA6            GPIO_NUM_0
#define LCD_IO_DATA7            GPIO_NUM_45
#define LCD_IO_DATA8            GPIO_NUM_48
#define LCD_IO_DATA9            GPIO_NUM_47
#define LCD_IO_DATA10           GPIO_NUM_21
#define LCD_IO_DATA11           GPIO_NUM_1
#define LCD_IO_DATA12           GPIO_NUM_2
#define LCD_IO_DATA13           GPIO_NUM_42
#define LCD_IO_DATA14           GPIO_NUM_41
#define LCD_IO_DATA15           GPIO_NUM_40
#define LCD_IO_DISP             (-1)

/* ============================================================
 * State
 * ============================================================ */

static i2c_master_dev_handle_t s_ch422g_io;
static i2c_master_dev_handle_t s_ch422g_cmd;
static esp_lcd_touch_handle_t  s_touch_handle;

/* ============================================================
 * CH422G I/O expander
 * ============================================================ */

static esp_err_t ch422g_init(i2c_master_bus_handle_t *bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CH422G_ADDR_IO,
        .scl_speed_hz    = I2C_EXPANDER_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(*bus, &cfg, &s_ch422g_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add CH422G IO device: %s", esp_err_to_name(ret));
        return ret;
    }

    cfg.device_address = CH422G_ADDR_CMD;
    ret = i2c_master_bus_add_device(*bus, &cfg, &s_ch422g_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add CH422G CMD device: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t ch422g_write(i2c_master_dev_handle_t dev, uint8_t cmd)
{
    return i2c_master_transmit(dev, &cmd, 1, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t ch422g_set_output_mode(void)
{
    return ch422g_write(s_ch422g_io, CH422G_CMD_OUT_EN);
}

/* ============================================================
 * Touch reset (GT911 + CH422G)
 * ============================================================ */

static esp_err_t touch_gpio_init(void)
{
    gpio_config_t cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = LCD_TOUCH_INT_SEL,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

static esp_err_t touch_reset(void)
{
    esp_err_t ret;

    ret = ch422g_set_output_mode();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH422G set output mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ch422g_write(s_ch422g_cmd, CH422G_CMD_RST_1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH422G reset cmd 1: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_rom_delay_us(100 * 1000);
    gpio_set_level(LCD_TOUCH_INT_GPIO, 0);
    esp_rom_delay_us(100 * 1000);

    ret = ch422g_write(s_ch422g_cmd, CH422G_CMD_RST_2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH422G reset cmd 2: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_rom_delay_us(200 * 1000);
    return ESP_OK;
}

/* ============================================================
 * GT911 touch controller
 * ============================================================ */

static esp_err_t touch_init(i2c_master_bus_handle_t *bus)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = I2C_TOUCH_FREQ_HZ;

    esp_err_t ret = esp_lcd_new_panel_io_i2c(*bus, &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_touch_config_t touch_cfg = {
        .x_max        = LCD_H_RES,
        .y_max        = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ret = esp_lcd_touch_new_i2c_gt911(io_handle, &touch_cfg, &s_touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_gt911: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/* ============================================================
 * RGB LCD panel
 * ============================================================ */

static esp_err_t lcd_panel_init(esp_lcd_panel_handle_t *out_panel)
{
    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz               = LCD_PIXEL_CLOCK_HZ,
            .h_res                 = LCD_H_RES,
            .v_res                 = LCD_V_RES,
            .hsync_pulse_width     = 4,
            .hsync_back_porch      = 8,
            .hsync_front_porch     = 8,
            .vsync_pulse_width     = 4,
            .vsync_back_porch      = 8,
            .vsync_front_porch     = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width            = LCD_RGB_DATA_WIDTH,
        .bits_per_pixel        = LCD_RGB_BITS_PER_PIXEL,
        .num_fbs               = LCD_NUM_FBS,
        .bounce_buffer_size_px = LCD_H_RES * LCD_BOUNCE_LINES,
        .sram_trans_align      = 4,
        .psram_trans_align     = 64,
        .hsync_gpio_num        = LCD_IO_HSYNC,
        .vsync_gpio_num        = LCD_IO_VSYNC,
        .de_gpio_num           = LCD_IO_DE,
        .pclk_gpio_num         = LCD_IO_PCLK,
        .disp_gpio_num         = LCD_IO_DISP,
        .data_gpio_nums = {
            LCD_IO_DATA0,  LCD_IO_DATA1,  LCD_IO_DATA2,  LCD_IO_DATA3,
            LCD_IO_DATA4,  LCD_IO_DATA5,  LCD_IO_DATA6,  LCD_IO_DATA7,
            LCD_IO_DATA8,  LCD_IO_DATA9,  LCD_IO_DATA10, LCD_IO_DATA11,
            LCD_IO_DATA12, LCD_IO_DATA13, LCD_IO_DATA14, LCD_IO_DATA15,
        },
        .flags.fb_in_psram = 1,
    };

    esp_err_t ret = esp_lcd_new_rgb_panel(&panel_cfg, out_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_init(*out_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/* ============================================================
 * LVGL port
 * ============================================================ */

static esp_err_t lvgl_port_bind(esp_lcd_panel_handle_t panel)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority     = 4,
        .task_stack        = 8 * 1024,
        .task_affinity     = -1,
        .task_max_sleep_ms = 10,
        .timer_period_ms   = 5,
    };

    esp_err_t ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init: %s", esp_err_to_name(ret));
        return ret;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram  = true,
            .buff_dma     = true,
            .direct_mode  = true,
            .full_refresh = false,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode       = true,
            .avoid_tearing = true,
        },
    };

    lv_display_t *display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (!display) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed");
        return ESP_FAIL;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = display,
        .handle = s_touch_handle,
    };

    lv_indev_t *touch_indev = lvgl_port_add_touch(&touch_cfg);
    if (!touch_indev) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}
/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t bsp_display_init(i2c_master_bus_handle_t *i2c_bus)
{
    if (!i2c_bus) return ESP_ERR_INVALID_ARG;

    esp_err_t ret;

    /* Order matters:
         expander → gpio → reset → touch → panel → lvgl
       Reset cannot run before the CH422G devices exist. */
    ret = ch422g_init(i2c_bus);
    if (ret != ESP_OK) return ret;

    ret = touch_gpio_init();
    if (ret != ESP_OK) return ret;

    ret = touch_reset();
    if (ret != ESP_OK) return ret;

    ret = touch_init(i2c_bus);
    if (ret != ESP_OK) return ret;

    esp_lcd_panel_handle_t panel = NULL;
    ret = lcd_panel_init(&panel);
    if (ret != ESP_OK) return ret;

    ret = lvgl_port_bind(panel);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Display stack initialized");
    return ESP_OK;
}

esp_err_t bsp_display_set_backlight(bool on)
{
    if (!s_ch422g_io || !s_ch422g_cmd) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ch422g_set_output_mode();
    if (ret != ESP_OK) return ret;

    uint8_t cmd = on ? CH422G_CMD_BL_ON : CH422G_CMD_BL_OFF;
    ret = ch422g_write(s_ch422g_cmd, cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set backlight: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Backlight: %s", on ? "ON" : "OFF");
    return ESP_OK;
}