#include "esp_wifi.h"
#include <stdbool.h>
#include <string.h>

#define MAX_SCAN_APS 20

static esp_err_t s_connect_result = ESP_OK;
static esp_err_t s_start_result   = ESP_OK;
static esp_err_t s_scan_result    = ESP_OK;
static bool      s_ap_info_valid;
static char      s_ap_ssid[33];
static uint8_t   s_ap_channel;
static int8_t    s_ap_rssi;
static wifi_ap_record_t s_scan_aps[MAX_SCAN_APS];
static int              s_scan_count;
static int s_connect_calls;
static int s_disconnect_calls;

void mock_wifi_reset(void)
{
    s_connect_result = ESP_OK;
    s_start_result = ESP_OK;
    s_scan_result = ESP_OK;
    s_ap_info_valid = false;
    memset(s_ap_ssid, 0, sizeof(s_ap_ssid));
    s_ap_channel = 0; s_ap_rssi = 0;
    memset(s_scan_aps, 0, sizeof(s_scan_aps));
    s_scan_count = 0;
    s_connect_calls = 0; s_disconnect_calls = 0;
}
void mock_wifi_set_connect_result(esp_err_t r) { s_connect_result = r; }
void mock_wifi_set_start_result(esp_err_t r)   { s_start_result = r; }
void mock_wifi_set_scan_result(esp_err_t r)    { s_scan_result = r; }
void mock_wifi_set_ap_info(const char *ssid, uint8_t ch, int8_t rssi)
{
    strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
    s_ap_channel = ch; s_ap_rssi = rssi; s_ap_info_valid = true;
}
void mock_wifi_clear_ap_info(void) { s_ap_info_valid = false; }
void mock_wifi_add_scan_ap(const char *ssid, int8_t rssi)
{
    if (s_scan_count >= MAX_SCAN_APS) return;
    strncpy((char *)s_scan_aps[s_scan_count].ssid, ssid, 32);
    s_scan_aps[s_scan_count].rssi = rssi;
    s_scan_aps[s_scan_count].channel = 1;
    s_scan_count++;
}
int mock_wifi_connect_calls(void)    { return s_connect_calls; }
int mock_wifi_disconnect_calls(void) { return s_disconnect_calls; }

esp_err_t esp_wifi_init(const wifi_init_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int s) { (void)s; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int m)    { (void)m; return ESP_OK; }
esp_err_t esp_wifi_set_config(int i, wifi_config_t *c) { (void)i; (void)c; return ESP_OK; }
esp_err_t esp_wifi_start(void) { return s_start_result; }
esp_err_t esp_wifi_connect(void) { s_connect_calls++; return s_connect_result; }
esp_err_t esp_wifi_disconnect(void) { s_disconnect_calls++; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info)
{
    if (!s_ap_info_valid) return ESP_ERR_WIFI_STATE;
    strncpy((char *)info->ssid, s_ap_ssid, 32);
    info->channel = s_ap_channel;
    info->rssi = s_ap_rssi;
    return ESP_OK;
}
esp_err_t esp_wifi_scan_start(const void *c, int b) { (void)c; (void)b; return s_scan_result; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *num)
{
    if (s_scan_result != ESP_OK) return s_scan_result;
    *num = (uint16_t)s_scan_count;
    return ESP_OK;
}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *num, wifi_ap_record_t *recs)
{
    if (s_scan_result != ESP_OK) return s_scan_result;
    uint16_t n = *num < s_scan_count ? *num : (uint16_t)s_scan_count;
    memcpy(recs, s_scan_aps, n * sizeof(wifi_ap_record_t));
    *num = n;
    return ESP_OK;
}
