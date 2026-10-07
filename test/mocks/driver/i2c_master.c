#include "driver/i2c_master.h"
#include <string.h>

static uint8_t   s_regs[256];
static esp_err_t s_add_result      = ESP_OK;
static esp_err_t s_tx_result       = ESP_OK;
static esp_err_t s_tx_rx_result    = ESP_OK;
static int       s_write_count;

void mock_i2c_reset(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_add_result = ESP_OK;
    s_tx_result = ESP_OK;
    s_tx_rx_result = ESP_OK;
    s_write_count = 0;
}

void mock_i2c_set_add_result(esp_err_t r)              { s_add_result = r; }
void mock_i2c_set_transmit_result(esp_err_t r)         { s_tx_result = r; }
void mock_i2c_set_transmit_receive_result(esp_err_t r) { s_tx_rx_result = r; }

void mock_i2c_set_reg(uint8_t reg, uint8_t value) { s_regs[reg] = value; }
uint8_t mock_i2c_get_reg(uint8_t reg)             { return s_regs[reg]; }

void mock_i2c_set_regs(uint8_t start, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && (start + i) < 256; i++)
        s_regs[start + i] = data[i];
}
void mock_i2c_get_regs(uint8_t start, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len && (start + i) < 256; i++)
        out[i] = s_regs[start + i];
}

int mock_i2c_get_write_count(void) { return s_write_count; }

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out)
{
    (void)bus;
    if (s_add_result != ESP_OK) return s_add_result;
    *out = (i2c_master_dev_handle_t)(uintptr_t)cfg->device_address;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
                              const uint8_t *data, size_t len,
                              int timeout_ms)
{
    (void)dev; (void)timeout_ms;
    if (s_tx_result != ESP_OK) return s_tx_result;
    if (len < 1) return ESP_ERR_INVALID_ARG;

    uint8_t reg = data[0];
    for (size_t i = 1; i < len && (reg + i - 1) < 256; i++)
        s_regs[reg + i - 1] = data[i];

    s_write_count++;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *write, size_t write_len,
                                      uint8_t *read, size_t read_len,
                                      int timeout_ms)
{
    (void)dev; (void)timeout_ms;
    if (s_tx_rx_result != ESP_OK) return s_tx_rx_result;
    if (write_len < 1) return ESP_ERR_INVALID_ARG;

    uint8_t reg = write[0];
    for (size_t i = 0; i < read_len && (reg + i) < 256; i++)
        read[i] = s_regs[reg + i];

    return ESP_OK;
}