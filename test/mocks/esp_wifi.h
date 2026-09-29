#ifndef ESP_WIFI_H
#define ESP_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_event.h"

#define ESP_ERR_WIFI_NOT_INIT    0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_STATE       0x3006
#define ESP_ERR_WIFI_CONN        0x3007
#define ESP_ERR_WIFI_SSID        0x300A

#define WIFI_EVENT_STA_START         0
#define WIFI_EVENT_STA_STOP          1
#define WIFI_EVENT_STA_CONNECTED     2
#define WIFI_EVENT_STA_DISCONNECTED  3
#define WIFI_EVENT_SCAN_DONE         6

#define IP_EVENT_STA_GOT_IP  0
#define IP_EVENT_STA_LOST_IP 1

#define WIFI_REASON_ASSOC_LEAVE             8
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_NO_AP_FOUND            201
#define WIFI_REASON_AUTH_FAIL              202

#define WIFI_IF_STA      0
#define WIFI_MODE_STA    1
#define WIFI_STORAGE_RAM 1

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA3_PSK,
} wifi_auth_mode_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct { uint8_t authmode; } threshold;
} wifi_sta_config_t;

typedef struct { wifi_sta_config_t sta; } wifi_config_t;

typedef struct {
    uint8_t ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;
    uint8_t authmode;
} wifi_ap_record_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t authmode;
} wifi_event_sta_connected_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t reason;
    int8_t  rssi;
} wifi_event_sta_disconnected_t;

typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){ .dummy = 0 })

esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *cfg);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info);
esp_err_t esp_wifi_scan_start(const void *config, int block);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *num);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *num, wifi_ap_record_t *recs);

void mock_wifi_reset(void);
void mock_wifi_set_connect_result(esp_err_t r);
void mock_wifi_set_start_result(esp_err_t r);
void mock_wifi_set_ap_info(const char *ssid, uint8_t channel, int8_t rssi);
void mock_wifi_clear_ap_info(void);
void mock_wifi_set_scan_result(esp_err_t r);
void mock_wifi_add_scan_ap(const char *ssid, int8_t rssi);
int  mock_wifi_connect_calls(void);
int  mock_wifi_disconnect_calls(void);

#endif
