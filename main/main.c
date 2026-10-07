#include "bsp_display.h"
#include "wifi_svc.h"
#include "ui.h"
#include "scd4x_svc.h"
#include "rtc_svc.h"
#include "esp_log.h"
#include <time.h>
#include "esp_lvgl_port.h"  

#include "driver/i2c_master.h"

#define TAG "MAIN"

extern void i2c_scan_bus(i2c_master_bus_handle_t bus);

i2c_master_bus_handle_t i2c_bus_handle;

static esp_err_t i2c_setup(i2c_master_bus_handle_t *bus)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port                     = 0,
        .sda_io_num                   = 8,
        .scl_io_num                   = 9,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C bus initialized on SDA=%d, SCL=%d", 8, 9);
    return ESP_OK;
}

/* ---- Таймеры LVGL ---- */

static void clock_poll_cb(lv_timer_t *t)
{
    (void)t;
    ui_update_clock();
}

static void sensor_poll_cb(lv_timer_t *t)
{
    (void)t;
    scd4x_data_t d;
    if (scd4x_svc_take_latest(&d)) {
        ui_update_sensors(d.co2, d.temperature, d.humidity);
    }
}

static void on_sntp_synced(void)
{
    rtc_svc_set_from_system();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Begin");

    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_ret);
    }

    setenv("TZ", "MSK-3", 1);
    tzset();

    ESP_ERROR_CHECK(i2c_setup(&i2c_bus_handle));

    if (rtc_svc_init(i2c_bus_handle) != ESP_OK) {
    ESP_LOGW(TAG, "Running without DS3231");
    }

    if (scd4x_svc_setup(i2c_bus_handle) == ESP_OK) {
        if (scd4x_svc_start(5000) != ESP_OK) {
            ESP_LOGW(TAG, "scd4x_svc_start failed");
        }
    } else {
        ESP_LOGW(TAG, "Running without SCD4X");
    }

    ESP_ERROR_CHECK(bsp_display_init(&i2c_bus_handle));

  

    ui_create();


    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_SVC_EVENTS, ESP_EVENT_ANY_ID,
        &ui_wifi_event_handler, NULL, NULL));

    wifi_svc_set_sntp_sync_cb(on_sntp_synced);    
    esp_err_t wifi_ret = wifi_svc_auto_connect();
    if (wifi_ret != ESP_OK && wifi_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "wifi_svc_auto_connect: %s", esp_err_to_name(wifi_ret));
    }

    if (lvgl_port_lock(0)) {
        lv_timer_create(clock_poll_cb,  1000, NULL);
        lv_timer_create(sensor_poll_cb, 1000, NULL);
        lvgl_port_unlock();
    }

    vTaskDelete(NULL);
}