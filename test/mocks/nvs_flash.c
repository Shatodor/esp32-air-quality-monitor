#include "nvs_flash.h"
#include <string.h>

#define MAX_ENTRIES 32
#define MAX_KEY_LEN 32
#define MAX_VAL_LEN 128

typedef struct {
    int  used;
    char key[MAX_KEY_LEN];
    char value[MAX_VAL_LEN];
} nvs_entry_t;

static nvs_entry_t s_store[MAX_ENTRIES];
static esp_err_t   s_open_result    = ESP_OK;
static esp_err_t   s_set_str_result = ESP_OK;
static esp_err_t   s_commit_result  = ESP_OK;
static int         s_write_count;

void mock_nvs_reset(void)
{
    memset(s_store, 0, sizeof(s_store));
    s_open_result = ESP_OK;
    s_set_str_result = ESP_OK;
    s_commit_result = ESP_OK;
    s_write_count = 0;
}
void mock_nvs_set_open_result(esp_err_t r)    { s_open_result = r; }
void mock_nvs_set_set_str_result(esp_err_t r) { s_set_str_result = r; }
void mock_nvs_set_commit_result(esp_err_t r)  { s_commit_result = r; }
int  mock_nvs_get_write_count(void)           { return s_write_count; }

esp_err_t nvs_flash_init(void)  { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name; (void)mode;
    if (s_open_result != ESP_OK) return s_open_result;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    if (s_set_str_result != ESP_OK) return s_set_str_result;
    if (strlen(key) >= MAX_KEY_LEN || strlen(value) >= MAX_VAL_LEN)
        return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_store[i].used && strcmp(s_store[i].key, key) == 0) {
            strncpy(s_store[i].value, value, MAX_VAL_LEN - 1);
            s_write_count++;
            return ESP_OK;
        }
    }
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!s_store[i].used) {
            s_store[i].used = 1;
            strncpy(s_store[i].key, key, MAX_KEY_LEN - 1);
            strncpy(s_store[i].value, value, MAX_VAL_LEN - 1);
            s_write_count++;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_store[i].used && strcmp(s_store[i].key, key) == 0) {
            size_t need = strlen(s_store[i].value) + 1;
            if (*len < need) { *len = need; return ESP_ERR_INVALID_SIZE; }
            memcpy(out, s_store[i].value, need);
            *len = need;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return s_commit_result; }
