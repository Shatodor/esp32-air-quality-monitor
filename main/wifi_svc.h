#ifndef WIFI_SVC_H
#define WIFI_SVC_H

/*
 * WiFi service (STA mode).
 *
 * Public API: connect, scan, auto-connect, credential storage (NVS),
 * NTP synchronization. Async variants return immediately — the result
 * is delivered via events.
 *
 * Events: WIFI_SVC_EVENTS (see wifi_event_id_t).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASS_MAX_LEN     64
#define WIFI_SSID_BUF_SIZE    (WIFI_SSID_MAX_LEN + 1)
#define WIFI_PASS_BUF_SIZE    (WIFI_PASS_MAX_LEN + 1)
#define WIFI_SCAN_MAX_RESULTS 20

ESP_EVENT_DECLARE_BASE(WIFI_SVC_EVENTS);

typedef enum {
    MSG_WIFI_SCANNING,
    MSG_WIFI_CONNECTING,
    MSG_WIFI_CONNECTED,
    MSG_WIFI_DISCONNECTED,
    MSG_WIFI_SCAN_SUCCESS,
    MSG_WIFI_SCAN_FAILED
} wifi_event_id_t;

/* ---- Sync ---- */
esp_err_t wifi_svc_connect(const char *ssid, const char *password);
esp_err_t wifi_svc_auto_connect(void);
esp_err_t wifi_svc_scan(char ssids[][WIFI_SSID_BUF_SIZE], size_t capacity, size_t *count);

/* ---- Status ---- */
bool wifi_svc_is_connected(void);

/* ---- Creds getters ---- */
esp_err_t wifi_svc_get_current_ssid(char *out, size_t max_len);
esp_err_t wifi_svc_get_target_ssid(char *out, size_t max_len);
bool wifi_svc_get_saved_password(const char *ssid, char *out_password, size_t max_len);

/* ---- Async ---- */
esp_err_t wifi_svc_connect_async(const char *ssid, const char *password);
esp_err_t wifi_svc_scan_async(void);
size_t wifi_svc_scan_get_results(char ssids[][WIFI_SSID_BUF_SIZE], size_t capacity);

#endif // WIFI_SVC_H