#include "wifi_creds.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

#define TAG "WIFI_CREDS"
#define NVS_NAMESPACE_WIFI "wifi_db"
#define WIFI_CREDS_KEY_LEN 16

/* Wide enough for any password we might store (WIFI_PASS_MAX_LEN is 64). */
#define WIFI_CREDS_PASS_BUF 128

static void make_key(const char *ssid, char out[WIFI_CREDS_KEY_LEN])
{
    if (!ssid) {
        out[0] = '\0';
        return;
    }
    uint32_t h = esp_crc32_le(0, (const uint8_t *)ssid, strlen(ssid));
    snprintf(out, WIFI_CREDS_KEY_LEN, "%08lx", (unsigned long)h);
}

bool wifi_creds_get(const char *ssid, char *out_password, size_t max_len)
{
    if (!ssid || !out_password || max_len == 0) return false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open readonly: %s", esp_err_to_name(err));
        }
        return false;
    }

    char key[WIFI_CREDS_KEY_LEN];
    make_key(ssid, key);

    size_t required_len = max_len;
    err = nvs_get_str(nvs, key, out_password, &required_len);
    nvs_close(nvs);

    if (err == ESP_OK) return true;
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str('%s'): %s", ssid, esp_err_to_name(err));
    }
    return false;
}

void wifi_creds_save(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !password) return;

    char existing[WIFI_CREDS_PASS_BUF] = {0};
    if (wifi_creds_get(ssid, existing, sizeof(existing))) {
        if (strcmp(existing, password) == 0) {
            ESP_LOGD(TAG, "Password for '%s' unchanged", ssid);
            return;
        }
        ESP_LOGI(TAG, "Password for '%s' changed, updating", ssid);
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    char key[WIFI_CREDS_KEY_LEN];
    make_key(ssid, key);

    err = nvs_set_str(nvs, key, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_str('%s'): %s", ssid, esp_err_to_name(err));
        nvs_close(nvs);
        return;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Password for '%s' saved (key=%s)", ssid, key);
}

esp_err_t wifi_creds_forget(const char *ssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    char key[WIFI_CREDS_KEY_LEN];
    make_key(ssid, key);

    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Nothing to erase — treat as success. */
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_erase_key('%s'): %s", ssid, esp_err_to_name(err));
    }

    if (err == ESP_OK) {
        esp_err_t commit_err = nvs_commit(nvs);
        if (commit_err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_commit: %s", esp_err_to_name(commit_err));
            err = commit_err;
        }
    }

    nvs_close(nvs);
    return err;
}