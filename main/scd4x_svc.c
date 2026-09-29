#include "scd4x_svc.h"
#include "scd4x.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "SCD4X_SVC"

static scd4x_t *s_scd4x = NULL;
static TaskHandle_t s_task = NULL;

static SemaphoreHandle_t s_data_lock = NULL;
static scd4x_data_t s_latest = {0};

bool scd4x_svc_is_ready(void)
{
    return s_scd4x != NULL;
}

bool scd4x_svc_take_latest(scd4x_data_t *out)
{
    if (!out || !s_data_lock) return false;

    xSemaphoreTake(s_data_lock, portMAX_DELAY);

    bool has_new = s_latest.valid;
    if (has_new) {
        *out = s_latest;
        s_latest.valid = false;   /* помечаем прочитанным */
    }
    xSemaphoreGive(s_data_lock);

    return has_new;
}

static esp_err_t register_device(i2c_master_bus_handle_t bus,
                                 i2c_master_dev_handle_t *out_dev)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SCD4X_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_config, out_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void log_sensor_info(scd4x_t *s)
{
    uint16_t serial[3];
    if (scd4x_get_serial_number(s, serial) == ESP_OK) {
        ESP_LOGI(TAG, "Serial: 0x%04X%04X%04X", serial[0], serial[1], serial[2]);
    } else {
        ESP_LOGW(TAG, "get_serial_number failed");
    }

    float temp_offset;
    if (scd4x_get_temperature_offset(s, &temp_offset) == ESP_OK) {
        ESP_LOGI(TAG, "Temperature offset: %.2f C", temp_offset);
    } else {
        ESP_LOGW(TAG, "get_temperature_offset failed");
    }

    uint16_t altitude;
    if (scd4x_get_sensor_altitude(s, &altitude) == ESP_OK) {
        ESP_LOGI(TAG, "Sensor altitude: %u m", altitude);
    } else {
        ESP_LOGW(TAG, "get_sensor_altitude failed");
    }

    bool asc;
    if (scd4x_get_automatic_self_calibration(s, &asc) == ESP_OK) {
        ESP_LOGI(TAG, "ASC: %s", asc ? "enabled" : "disabled");
    } else {
        ESP_LOGW(TAG, "get_automatic_self_calibration failed");
    }
}

esp_err_t scd4x_svc_setup(i2c_master_bus_handle_t bus)
{
    if (s_scd4x) return ESP_OK;   /* уже инициализирован */

    if (!s_data_lock) {
        s_data_lock = xSemaphoreCreateMutex();
        if (!s_data_lock) return ESP_ERR_NO_MEM;
    }

    i2c_master_dev_handle_t dev;
    esp_err_t ret = register_device(bus, &dev);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Initializing SCD4X...");
    s_scd4x = scd4x_init(dev);
    if (!s_scd4x) {
        ESP_LOGE(TAG, "scd4x_init failed");
        return ESP_ERR_NOT_FOUND;
    }

    log_sensor_info(s_scd4x);

    ret = scd4x_start_periodic_measurement(s_scd4x);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_periodic_measurement: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Periodic measurement started");
    return ESP_OK;
}

static void scd4x_task(void *pv)
{
    uint32_t period_ms = (uint32_t)(uintptr_t)pv;

    for (;;) {
        if (s_scd4x) {
            scd4x_measurement_t m = {0};
            esp_err_t ret = scd4x_read_measurement(s_scd4x, &m);

            if (ret == ESP_OK) {
                xSemaphoreTake(s_data_lock, portMAX_DELAY);
                s_latest.co2         = m.co2;
                s_latest.temperature = m.temperature;
                s_latest.humidity    = m.humidity;
                s_latest.valid       = true;   /* «есть непрочитанное» */
                xSemaphoreGive(s_data_lock);
            } else if (ret != ESP_ERR_NOT_FINISHED) {
                ESP_LOGE(TAG, "read_measurement: %s", esp_err_to_name(ret));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

esp_err_t scd4x_svc_start(uint32_t period_ms)
{
    if (s_task) return ESP_OK;   /* уже запущена */
    if (period_ms < 1000) period_ms = 1000;

    BaseType_t ret = xTaskCreate(scd4x_task, "scd4x",
                                 4096, (void *)(uintptr_t)period_ms,
                                 4, &s_task);
    if (ret != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}