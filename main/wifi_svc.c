/*
 * WiFi service (STA mode).
 *
 * Responsibilities:
 *   - connecting to an access point (sync + async API);
 *   - auto-connect to a saved network;
 *   - scanning the air and caching the results;
 *   - reconnecting on link loss (up to MAX_RECONNECT_ATTEMPTS);
 *   - storing credentials in NVS (namespace "wifi_db");
 *   - NTP synchronization once an IP is obtained.
 *
 * Publishes events via WIFI_SVC_EVENTS.
 */

#include "wifi_svc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "esp_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define TAG "WIFI_SVC"
#define MAX_RECONNECT_ATTEMPTS 5
#define NVS_NAMESPACE_WIFI "wifi_db"

#define WIFI_DISCONNECTED_BIT BIT0

ESP_EVENT_DEFINE_BASE(WIFI_SVC_EVENTS);

static bool s_wifi_initialized;
static atomic_bool s_wifi_connected;

static uint8_t s_reconnect_cnt;
static esp_netif_t *s_sta_netif;

static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_scan_lock;
static EventGroupHandle_t s_events;

static char s_scan_results[WIFI_SCAN_MAX_RESULTS][WIFI_SSID_BUF_SIZE];
static char s_pending_ssid[WIFI_SSID_BUF_SIZE];
static char s_pending_password[WIFI_PASS_BUF_SIZE];
static char s_connected_ssid[WIFI_SSID_BUF_SIZE];
static char s_connected_password[WIFI_PASS_BUF_SIZE];
static size_t s_scan_count;

typedef struct {
    char ssid[WIFI_SSID_BUF_SIZE];
    char password[WIFI_PASS_BUF_SIZE];
} connect_req_t;

static bool s_connecting;
static bool s_scanning;

static wifi_sntp_sync_cb_t s_sntp_sync_cb = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

/* ---- Helpers: locking ---- */

static inline void state_lock(void)
{
    if (s_state_lock) xSemaphoreTake(s_state_lock, portMAX_DELAY);
}

static inline void state_unlock(void)
{
    if (s_state_lock) xSemaphoreGive(s_state_lock);
}

static void post_ui_event(wifi_event_id_t id)
{
    esp_err_t e = esp_event_post(WIFI_SVC_EVENTS, (int32_t)id, NULL, 0, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "post event %d failed: %s", (int)id, esp_err_to_name(e));
    }
}

/* ---- Getters ---- */

static esp_err_t copy_ssid_locked(const char *src, char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;

    state_lock();
    bool has_ssid = (src[0] != '\0');
    if (has_ssid) {
        strncpy(out, src, max_len - 1);
        out[max_len - 1] = '\0';
    } else {
        out[0] = '\0';
    }
    state_unlock();

    return has_ssid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_svc_get_current_ssid(char *out, size_t max_len)
{
    return copy_ssid_locked(s_connected_ssid, out, max_len);
}

esp_err_t wifi_svc_get_target_ssid(char *out, size_t max_len)
{
    return copy_ssid_locked(s_pending_ssid, out, max_len);
}

/* ---- NTP ---- */


static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "NTP: time synced");

    if (s_sntp_sync_cb) {
        s_sntp_sync_cb();
    }
}

void wifi_svc_set_sntp_sync_cb(wifi_sntp_sync_cb_t cb)
{
    s_sntp_sync_cb = cb;
}

/* ---- NVS helpers ---- */

static void make_nvs_key(const char *ssid, char out[16])
{
    if (!ssid) {
        out[0] = '\0';
        return;
    }
    uint32_t h = esp_crc32_le(0, (const uint8_t *)ssid, strlen(ssid));
    snprintf(out, 16, "%08lx", (unsigned long)h);
}

static void save_password_to_db(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !password) {
        return;
    }

    char existing[WIFI_PASS_BUF_SIZE] = {0};
    if (wifi_svc_get_saved_password(ssid, existing, sizeof(existing))) {
        if (strcmp(existing, password) == 0) {
            ESP_LOGD(TAG, "Password '%s' already saved, returning", ssid);
            return;
        }
        ESP_LOGI(TAG, "Password '%s' changed, updating", ssid);
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    char nvs_key[16];
    make_nvs_key(ssid, nvs_key);

    err = nvs_set_str(nvs, nvs_key, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_str failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Password for SSID '%s' saved (key=%s)", ssid, nvs_key);
}

bool wifi_svc_get_saved_password(const char *ssid, char *out_password, size_t max_len)
{
    if (!ssid || !out_password || max_len == 0) return false;

    nvs_handle_t nvs;
    bool found = false;

    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open readonly: %s", esp_err_to_name(err));
        }
        return false;
    }

    char nvs_key[16];
    make_nvs_key(ssid, nvs_key);

    size_t required_len = max_len;
    err = nvs_get_str(nvs, nvs_key, out_password, &required_len);
    if (err == ESP_OK) {
        found = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
    return found;
}

/* ---- Init ---- */

static esp_err_t wifi_svc_init(void)
{
    if (s_wifi_initialized) return ESP_OK;

    if (s_state_lock == NULL) s_state_lock = xSemaphoreCreateMutex();
    if (s_scan_lock == NULL)  s_scan_lock  = xSemaphoreCreateMutex();
    if (s_events == NULL)     s_events     = xEventGroupCreate();
    if (!s_state_lock || !s_scan_lock || !s_events) return ESP_ERR_NO_MEM;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), erasing...", esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            ESP_LOGE(TAG, "create_default_wifi_sta failed");
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST(
            "ru.pool.ntp.org",
            "time.google.com",
            "time.cloudflare.com"
        ));
    sntp_cfg.start         = true;
    sntp_cfg.smooth_sync   = false;
    sntp_cfg.wait_for_sync = false;
    sntp_cfg.sync_cb       = sntp_sync_cb;

    esp_err_t sntp_ret = esp_netif_sntp_init(&sntp_cfg);
    if (sntp_ret != ESP_OK) {
        ESP_LOGW(TAG, "NTP: init failed: %s", esp_err_to_name(sntp_ret));
    } else {
        ESP_LOGI(TAG, "NTP: started (esp_netif_sntp)");
    }

    s_wifi_initialized = true;
    return ESP_OK;
}

/* ---- Event handler ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        bool have_pending_ssid;
        state_lock();
        have_pending_ssid = (s_pending_ssid[0] != '\0');
        if (have_pending_ssid) s_reconnect_cnt = 0;
        state_unlock();

        if (have_pending_ssid) {
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "STA_START connect: %s", esp_err_to_name(ret));
            }
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)event_data;

        size_t len = e->ssid_len;
        if (len >= sizeof(s_connected_ssid)) {
            ESP_LOGW(TAG, "STA_CONNECTED: bad ssid_len=%u, clamping", (unsigned)len);
            len = sizeof(s_connected_ssid) - 1;
        }

        char log_ssid[WIFI_SSID_BUF_SIZE];
        uint8_t bssid[6];
        uint8_t channel;

        state_lock();
        memcpy(s_connected_ssid, e->ssid, len);
        s_connected_ssid[len] = '\0';
        strncpy(s_connected_password, s_pending_password, sizeof(s_connected_password) - 1);
        s_connected_password[sizeof(s_connected_password) - 1] = '\0';
        memcpy(log_ssid, s_connected_ssid, sizeof(log_ssid));
        memcpy(bssid, e->bssid, sizeof(bssid));
        channel = e->channel;
        state_unlock();

        ESP_LOGI(TAG, "Associated with '%s' (bssid=%02x:%02x:%02x:%02x:%02x:%02x, ch=%d)",
                 log_ssid, bssid[0], bssid[1], bssid[2],
                 bssid[3], bssid[4], bssid[5], channel);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;

        bool give_up = false;
        bool has_target = false;
        bool voluntary = (d->reason == WIFI_REASON_ASSOC_LEAVE);
        char log_ssid[WIFI_SSID_BUF_SIZE];

        state_lock();
        memcpy(log_ssid, s_connected_ssid, sizeof(log_ssid));
        atomic_store(&s_wifi_connected, false);
        s_connected_ssid[0] = '\0';
        s_connected_password[0] = '\0';

        if (s_pending_ssid[0]) {
            if (voluntary) {
                has_target = true;
            } else if (s_reconnect_cnt < MAX_RECONNECT_ATTEMPTS) {
                s_reconnect_cnt++;
                has_target = true;
            } else {
                s_pending_ssid[0] = '\0';
                give_up = true;
            }
        }
        state_unlock();

        ESP_LOGW(TAG, "DISCONNECTED, reason=%d, rssi=%d, ssid='%s'",
                 d->reason, d->rssi, log_ssid);

        if (s_events) {
            xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
        }

        if (give_up) {
            post_ui_event(MSG_WIFI_DISCONNECTED);
        } else if (has_target) {
            esp_err_t e = esp_wifi_connect();
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(e));
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        char ssid_copy[WIFI_SSID_BUF_SIZE];
        char pass_copy[WIFI_PASS_BUF_SIZE];

        state_lock();
        atomic_store(&s_wifi_connected, true);
        s_reconnect_cnt = 0;
        strncpy(ssid_copy, s_connected_ssid, sizeof(ssid_copy) - 1);
        ssid_copy[sizeof(ssid_copy) - 1] = '\0';
        strncpy(pass_copy, s_connected_password, sizeof(pass_copy) - 1);
        pass_copy[sizeof(pass_copy) - 1] = '\0';
        state_unlock();

        if (s_events) {
            xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        }

        save_password_to_db(ssid_copy, pass_copy);
        post_ui_event(MSG_WIFI_CONNECTED);

        ESP_LOGI(TAG, "Got IP: " IPSTR " (ssid='%s')",
                 IP2STR(&event->ip_info.ip), ssid_copy);
    }
}

/* ---- Start connection (sync) ---- */

esp_err_t wifi_svc_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(s_pending_ssid))
        return ESP_ERR_INVALID_ARG;

    if (!password) password = "";
    if (strlen(password) >= sizeof(s_pending_password)) {
        ESP_LOGE(TAG, "Password too long");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wifi_svc_init();
    if (ret != ESP_OK) return ret;

    state_lock();
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    strncpy(s_pending_password, password, sizeof(s_pending_password) - 1);
    s_pending_password[sizeof(s_pending_password) - 1] = '\0';
    s_connected_ssid[0] = '\0';
    s_connected_password[0] = '\0';
    atomic_store(&s_wifi_connected, false);
    s_reconnect_cnt  = 0;
    state_unlock();

    wifi_config_t config = {0};
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    strncpy((char *)config.sta.ssid,     ssid,     sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, password, sizeof(config.sta.password) - 1);

    ESP_LOGI(TAG, "Connecting to SSID: %s", config.sta.ssid);

    ret = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_config: %s", esp_err_to_name(ret));
        return ret;
    }

    post_ui_event(MSG_WIFI_CONNECTING);

    ret = esp_wifi_start();
    ESP_LOGD(TAG, "esp_wifi_start -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) return ret;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "Already on '%s', disconnecting first", ap_info.ssid);
        return esp_wifi_disconnect();
    }

    ret = esp_wifi_connect();
    if (ret == ESP_ERR_WIFI_CONN) {
        ESP_LOGI(TAG, "Connect busy, forcing disconnect to switch target");
        esp_wifi_disconnect();
        return ESP_OK;
    }
    return ret;
}

/* ---- Auto-connect ---- */

esp_err_t wifi_svc_auto_connect(void)
{
    esp_err_t ret = wifi_svc_init();
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) return ret;

    ESP_LOGI(TAG, "Auto-connect: scanning...");
    ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) return ret;

    uint16_t found = 0;
    ret = esp_wifi_scan_get_ap_num(&found);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "scan_get_ap_num: %s", esp_err_to_name(ret));
        return ret;
    }
    if (found == 0) {
        ESP_LOGW(TAG, "No available SSIDs");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(*records));
    if (!records) return ESP_ERR_NO_MEM;

    ret = esp_wifi_scan_get_ap_records(&found, records);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "scan_get_ap_records: %s", esp_err_to_name(ret));
        free(records);
        return ret;
    }

    char best_ssid[WIFI_SSID_BUF_SIZE] = {0};
    char saved_password[WIFI_PASS_BUF_SIZE] = {0};
    bool saved_ssid_found = false;

    for (int i = 0; i < found; i++) {
        char temp_ssid[WIFI_SSID_BUF_SIZE];
        strncpy(temp_ssid, (const char *)records[i].ssid, WIFI_SSID_MAX_LEN);
        temp_ssid[WIFI_SSID_MAX_LEN] = '\0';

        if (wifi_svc_get_saved_password(temp_ssid, saved_password, sizeof(saved_password))) {
            strncpy(best_ssid, temp_ssid, sizeof(best_ssid) - 1);
            best_ssid[sizeof(best_ssid) - 1] = '\0';
            saved_ssid_found = true;
            ESP_LOGI(TAG, "Found SSID: %s (RSSI: %d dBm)", best_ssid, records[i].rssi);
            break;
        }
    }

    free(records);

    if (saved_ssid_found) {
        return wifi_svc_connect(best_ssid, saved_password);
    }

    ESP_LOGW(TAG, "Saved SSIDs not found");
    return ESP_ERR_NOT_FOUND;
}

/* ---- Scan (sync) ---- */

esp_err_t wifi_svc_scan(char ssids[][WIFI_SSID_BUF_SIZE], size_t capacity, size_t *count)
{
    if (!ssids || !count || capacity == 0) return ESP_ERR_INVALID_ARG;

    *count = 0;

    esp_err_t ret = wifi_svc_init();
    if (ret != ESP_OK) return ret;

    state_lock();
    s_pending_ssid[0] = '\0';
    s_pending_password[0] = '\0';
    s_connected_ssid[0] = '\0';
    s_connected_password[0] = '\0';
    atomic_store(&s_wifi_connected, false);
    s_reconnect_cnt = 0;
    state_unlock();

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) return ret;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        if (s_events) {
            xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        }

        esp_err_t disc_ret = esp_wifi_disconnect();
        if (disc_ret != ESP_OK) {
            ESP_LOGW(TAG, "Disconnect before scan failed: %s", esp_err_to_name(disc_ret));
        } else if (s_events) {
            EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_DISCONNECTED_BIT,
                                                   pdTRUE, pdFALSE,
                                                   pdMS_TO_TICKS(3000));
            if (!(bits & WIFI_DISCONNECTED_BIT)) {
                ESP_LOGW(TAG, "Disconnect timeout before scan, proceeding anyway");
            }
        }
    }

    ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) return ret;

    uint16_t found = 0;
    ret = esp_wifi_scan_get_ap_num(&found);
    if (ret != ESP_OK) return ret;

    wifi_ap_record_t *records = calloc(found ? found : 1, sizeof(*records));
    if (!records) return ESP_ERR_NO_MEM;

    ret = esp_wifi_scan_get_ap_records(&found, records);
    if (ret != ESP_OK) {
        free(records);
        return ret;
    }
    *count = found < capacity ? found : capacity;

    for (size_t i = 0; i < *count; i++) {
        strncpy(ssids[i], (const char *)records[i].ssid, WIFI_SSID_MAX_LEN);
        ssids[i][WIFI_SSID_MAX_LEN] = '\0';
    }

    free(records);
    return ESP_OK;
}

/* ---- Status ---- */

bool wifi_svc_is_connected(void)
{
    return atomic_load(&s_wifi_connected);
}

/* ---- Async API ---- */

size_t wifi_svc_scan_get_results(char ssids[][WIFI_SSID_BUF_SIZE], size_t capacity)
{
    if (!ssids || capacity == 0 || !s_scan_lock) return 0;

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    size_t n = s_scan_count < capacity ? s_scan_count : capacity;
    for (size_t i = 0; i < n; i++) {
        strncpy(ssids[i], s_scan_results[i], WIFI_SSID_MAX_LEN);
        ssids[i][WIFI_SSID_MAX_LEN] = '\0';
    }
    xSemaphoreGive(s_scan_lock);
    return n;
}

static void scan_async_task(void *pv)
{
    (void)pv;

    post_ui_event(MSG_WIFI_SCANNING);

    char local_results[WIFI_SCAN_MAX_RESULTS][WIFI_SSID_BUF_SIZE];
    size_t count = 0;
    esp_err_t res = wifi_svc_scan(local_results, WIFI_SCAN_MAX_RESULTS, &count);

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (res == ESP_OK) {
        memcpy(s_scan_results, local_results, sizeof(s_scan_results));
        s_scan_count = count;
    } else {
        s_scan_count = 0;
    }
    xSemaphoreGive(s_scan_lock);

    state_lock();
    s_scanning = false;
    state_unlock();

    post_ui_event(res == ESP_OK ? MSG_WIFI_SCAN_SUCCESS : MSG_WIFI_SCAN_FAILED);
    vTaskDelete(NULL);
}

esp_err_t wifi_svc_scan_async(void)
{
    esp_err_t ret = wifi_svc_init();
    if (ret != ESP_OK) return ret;

    if (!s_scan_lock) {
        ESP_LOGE(TAG, "Scan lock missing after init");
        return ESP_ERR_INVALID_STATE;
    }

    state_lock();
    if (s_scanning) {
        state_unlock();
        ESP_LOGW(TAG, "Scan already in progress, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    s_scanning = true;
    state_unlock();

    BaseType_t task_ret = xTaskCreate(scan_async_task, "wifi_svc_scan", 4096, NULL, 3, NULL);
    if (task_ret != pdPASS) {
        state_lock();
        s_scanning = false;
        state_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Async connect ---- */

static void connect_async_task(void *pv)
{
    connect_req_t *req = (connect_req_t *)pv;
    wifi_svc_connect(req->ssid, req->password);
    free(req);

    state_lock();
    s_connecting = false;
    state_unlock();

    vTaskDelete(NULL);
}

esp_err_t wifi_svc_connect_async(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!password) password = "";

    esp_err_t ret = wifi_svc_init();
    if (ret != ESP_OK) return ret;

    state_lock();
    if (s_connecting) {
        state_unlock();
        ESP_LOGW(TAG, "Connect already in progress, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    s_connecting = true;
    state_unlock();

    connect_req_t *req = malloc(sizeof(*req));
    if (!req) {
        state_lock();
        s_connecting = false;
        state_unlock();
        return ESP_ERR_NO_MEM;
    }

    strncpy(req->ssid, ssid, sizeof(req->ssid) - 1);
    req->ssid[sizeof(req->ssid) - 1] = '\0';
    strncpy(req->password, password, sizeof(req->password) - 1);
    req->password[sizeof(req->password) - 1] = '\0';

    BaseType_t task_ret = xTaskCreate(connect_async_task, "wifi_svc_conn", 4096, req, 3, NULL);
    if (task_ret != pdPASS) {
        free(req);
        state_lock();
        s_connecting = false;
        state_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}