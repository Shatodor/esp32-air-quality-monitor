
#include "waveshare_rgb_lcd_port.h"
#include "lcd_touch.h"


esp_lcd_touch_handle_t touch_handle = NULL; // Declare a handle for the touch panel


// Initialize RGB LCD
esp_err_t waveshare_esp32_s3_rgb_lcd_init(i2c_master_bus_handle_t *i2c_bus_handle) {
    lcd_gpio_init();
    lcd_touch_reset();
    lcd_io_expander_init(i2c_bus_handle);
    lcd_touch_init(i2c_bus_handle, &touch_handle);
 
    ESP_LOGI("LCD", "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;     // Declare a handle for the LCD panel
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,             // Set the clock source for the panel
        .timings =  {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,  // Pixel clock frequency
            .h_res = EXAMPLE_LCD_H_RES,             // Horizontal resolution
            .v_res = EXAMPLE_LCD_V_RES,             // Vertical resolution
            .hsync_pulse_width = 4,                 // Horizontal sync pulse width
            .hsync_back_porch = 8,                  // Horizontal back porch
            .hsync_front_porch = 8,                 // Horizontal front porch
            .vsync_pulse_width = 4,                 // Vertical sync pulse width
            .vsync_back_porch = 8,                  // Vertical back porch
            .vsync_front_porch = 8,                 // Vertical front porch
            .flags = {
                .pclk_active_neg = 1,               // Active low pixel clock
            },
        },
        .data_width = EXAMPLE_RGB_DATA_WIDTH,           // Data width for RGB
        .bits_per_pixel = EXAMPLE_RGB_BIT_PER_PIXEL,    // Bits per pixel
        .num_fbs = 2,                                  // Two framebuffers for tear-free rendering
        .bounce_buffer_size_px = EXAMPLE_RGB_BOUNCE_BUFFER_SIZE, // Bounce buffer size in pixels
        .sram_trans_align = 4,                          // SRAM transaction alignment
        .psram_trans_align = 64,                        // PSRAM transaction alignment
        .hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC,     // GPIO number for horizontal sync
        .vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC,     // GPIO number for vertical sync
        .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,           // GPIO number for data enable
        .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK,       // GPIO number for pixel clock
        .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP,       // GPIO number for display
        .data_gpio_nums = {
            EXAMPLE_LCD_IO_RGB_DATA0,
            EXAMPLE_LCD_IO_RGB_DATA1,
            EXAMPLE_LCD_IO_RGB_DATA2,
            EXAMPLE_LCD_IO_RGB_DATA3,
            EXAMPLE_LCD_IO_RGB_DATA4,
            EXAMPLE_LCD_IO_RGB_DATA5,
            EXAMPLE_LCD_IO_RGB_DATA6,
            EXAMPLE_LCD_IO_RGB_DATA7,
            EXAMPLE_LCD_IO_RGB_DATA8,
            EXAMPLE_LCD_IO_RGB_DATA9,
            EXAMPLE_LCD_IO_RGB_DATA10,
            EXAMPLE_LCD_IO_RGB_DATA11,
            EXAMPLE_LCD_IO_RGB_DATA12,
            EXAMPLE_LCD_IO_RGB_DATA13,
            EXAMPLE_LCD_IO_RGB_DATA14,
            EXAMPLE_LCD_IO_RGB_DATA15,
        },
        .flags = {
            .fb_in_psram = 1, // Use PSRAM for framebuffer
        },
    };

    
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle)); // Create a new RGB panel with the specified configuration
    ESP_LOGI("LCD", "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY,
        .task_stack = CONFIG_EXAMPLE_LVGL_PORT_TASK_STACK_SIZE_KB * 1024,
        .task_affinity = CONFIG_EXAMPLE_LVGL_PORT_TASK_CORE,
        .task_max_sleep_ms = CONFIG_EXAMPLE_LVGL_PORT_TASK_MAX_DELAY_MS,
        .timer_period_ms = CONFIG_EXAMPLE_LVGL_PORT_TICK,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t display_cfg = {
        .panel_handle = panel_handle,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES,
        .double_buffer = true,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            .buff_dma = true,
            .direct_mode = true, //CHANGED
            .full_refresh = false, //ADDED
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };
    lv_display_t *display = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_ERR_NO_MEM);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = display,
        .handle = touch_handle,
    };
    ESP_ERROR_CHECK(lvgl_port_add_touch(&touch_cfg) ? ESP_OK : ESP_ERR_NO_MEM);

    return ESP_OK;
}

