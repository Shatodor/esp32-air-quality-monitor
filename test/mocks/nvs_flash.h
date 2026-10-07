#ifndef NVS_FLASH_H
#define NVS_FLASH_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;

typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);

void mock_nvs_reset(void);
void mock_nvs_set_open_result(esp_err_t r);
void mock_nvs_set_set_str_result(esp_err_t r);
void mock_nvs_set_commit_result(esp_err_t r);
int  mock_nvs_get_write_count(void);

#endif
