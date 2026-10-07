#ifndef I2C_MASTER_H
#define I2C_MASTER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7 = 0,
    I2C_ADDR_BIT_LEN_10,
} i2c_addr_bit_len_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t           device_address;
    uint32_t           scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
                              const uint8_t *data, size_t len,
                              int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *write, size_t write_len,
                                      uint8_t *read, size_t read_len,
                                      int timeout_ms);

/* ---- Test control ---- */
void mock_i2c_reset(void);
void mock_i2c_set_add_result(esp_err_t r);
void mock_i2c_set_transmit_result(esp_err_t r);
void mock_i2c_set_transmit_receive_result(esp_err_t r);

void    mock_i2c_set_reg(uint8_t reg, uint8_t value);
uint8_t mock_i2c_get_reg(uint8_t reg);
void    mock_i2c_set_regs(uint8_t start, const uint8_t *data, size_t len);
void    mock_i2c_get_regs(uint8_t start, uint8_t *out, size_t len);

int mock_i2c_get_write_count(void);

#endif