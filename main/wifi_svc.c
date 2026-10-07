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
#include "wifi_creds.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define TAG "WIFI_SVC"
#define WIFI_SVC_TASK_STACK   4096
#define WIFI_SVC_TASK_PRIO    3
#define MAX_RECONNECT_ATTEMPTS 5
#define WIFI_DISCONNECTED_BIT BIT0

ESP_EVENT_DEFINE_BASE(WIFI_SVC_EVENTS);

/* ---- Module state ---- */

static bool s_wifi_initialized;
static atomic_bool s_wifi_connected;
static atomic_int_least32_t s_last_disconnect_reason;

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

/* ---- WiFi state machine ---- */

typedef enum {
    WIFI_ST_IDLE,           /* not connected, not trying */
    WIFI_ST_SCANNING,       /* scan in progress */
    WIFI_ST_CONNECTING,     /* initial connection attempt */
    WIFI_ST_CONNECTED,      /* associated + IP */
    WIFI_ST_RECONNECTING,   /* link lost, auto-reconnect running */
    WIFI_ST_DISCONNECTING,  /* voluntary disconnect (switching networks) */
    WIFI_ST_FAILED,         /* all reconnect attempts exhausted */
} wifi_state_t;

static wifi_state_t s_state = WIFI_ST_IDLE;

static const char *wifi_state_name(wifi_state_t s)
{
    switch (s) {
    case WIFI_ST_IDLE:          return "IDLE";
    case WIFI_ST_SCANNING:      return "SCANNING";
    case WIFI_ST_CONNECTING:    return "CONNECTING";
    case WIFI_ST_CONNECTED:     return "CONNECTED";
    case WIFI_ST_RECONNECTING:  return "RECONNECTING";
    case WIFI_ST_DISCONNECTING: return "DISCONNECTING";
    case WIFI_ST_FAILED:        return "FAILED";
    default:                    return "?";
    }
}

/* ---- FSM events ---- */
typedef enum {
    FSM_EVT_STA_ASSOC,          /* WIFI_EVENT_STA_CONNECTED,    data: wifi_event_sta_connected_t *  */
    FSM_EVT_STA_DISCONNECTED,   /* WIFI_EVENT_STA_DISCONNECTED, data: wifi_event_sta_disconnected_t * */
    FSM_EVT_GOT_IP,             /* IP_EVENT_STA_GOT_IP,         data: ip_event_got_ip_t *            */
} fsm_evt_t;

static void fsm_dispatch     (fsm_evt_t evt, const void *data);
static void fsm_inactive     (fsm_evt_t evt, const void *data);  /* IDLE / FAILED */
static void fsm_scanning     (fsm_evt_t evt, const void *data);
static void fsm_active       (fsm_evt_t evt, const void *data);  /* CONNECTING / RECONNECTING */
static void fsm_connected    (fsm_evt_t evt, const void *data);
static void fsm_disconnecting(fsm_evt_t evt, const void *data);


/* Reasons that indicate bad credentials — retrying won't help. */
static bool reason_is_fatal(uint8_t reason)
{
    switch (reason) {
    case 15:   /* WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT */
    case 202:  /* WIFI_REASON_AUTH_FAIL */
    case 204:  /* WIFI_REASON_HANDSHAKE_TIMEOUT */
    case 205:  /* WIFI_REASON_CONNECTION_FAIL */
        return true;
    default:
        return false;
    }
}

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

/* Caller must hold s_state_lock. */
static void fsm_set_state(wifi_state_t next)
{
    if (s_state == next) return;
    ESP_LOGI(TAG, "%s -> %s", wifi_state_name(s_state), wifi_state_name(next));
    s_state = next;
}

/* ---- Credentials (public wrappers over wifi_creds) ---- */

bool wifi_svc_get_saved_password(const char *ssid, char *out_password, size_t max_len)
{
    return wifi_creds_get(ssid, out_password, max_len);
}

esp_err_t wifi_svc_forget(const char *ssid)
{
    return wifi_creds_forget(ssid);
}

/* ---- FSM: common GOT_IP path ---- */

/*
 * Commit connected state, persist credentials, notify UI.
 * Shared by fsm_active and fsm_disconnecting.
 */
static void fsm_handle_got_ip(const ip_event_got_ip_t *e)
{
    char ssid[WIFI_SSID_BUF_SIZE];
    char pass[WIFI_PASS_BUF_SIZE];

    state_lock();
    atomic_store(&s_wifi_connected, true);
    s_reconnect_cnt = 0;
    fsm_set_state(WIFI_ST_CONNECTED);
    strncpy(ssid, s_connected_ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(pass, s_connected_password, sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = '\0';
    state_unlock();

    if (s_events) xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);

    wifi_creds_save(ssid, pass);
    post_ui_event(MSG_WIFI_CONNECTED);

    ESP_LOGI(TAG, "Got IP: " IPSTR " (ssid='%s')",
             IP2STR(&e->ip_info.ip), ssid);
}

/* ---- FSM: dispatcher ---- */

/*
 * Dispatch only from the WiFi/IP event loop task.
 * API-side transitions (wifi_svc_connect, wifi_svc_scan_async) set the
 * state directly under s_state_lock instead of going through here.
 */
static void fsm_dispatch(fsm_evt_t evt, const void *data)
{
    wifi_state_t st;
    state_lock();
    st = s_state;
    state_unlock();

    switch (st) {
    case WIFI_ST_IDLE:
    case WIFI_ST_FAILED:        fsm_inactive      (evt, data); break;
    case WIFI_ST_SCANNING:      fsm_scanning      (evt, data); break;
    case WIFI_ST_CONNECTING:
    case WIFI_ST_RECONNECTING:  fsm_active        (evt, data); break;
    case WIFI_ST_CONNECTED:     fsm_connected     (evt, data); break;
    case WIFI_ST_DISCONNECTING: fsm_disconnecting (evt, data); break;
    }
}

static void fsm_inactive(fsm_evt_t evt, const void *data)
{
    (void)data;
    switch (evt) {
    case FSM_EVT_STA_DISCONNECTED:
        /* stray event from a previous session — just clear the flag */
        state_lock();
        atomic_store(&s_wifi_connected, false);
        s_connected_ssid[0] = '\0';
        s_connected_password[0] = '\0';
        state_unlock();
        break;
    default:
        break;
    }
}

static void fsm_scanning(fsm_evt_t evt, const void *data)
{
    (void)data;
    switch (evt) {
    case FSM_EVT_STA_DISCONNECTED:
        /* wifi_svc_scan() disconnects before scanning; this is expected.
           Signal the scan task's event-group wait and continue. */
        state_lock();
        atomic_store(&s_wifi_connected, false);
        state_unlock();
        if (s_events) xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
        break;
    default:
        break;
    }
}

static void fsm_active(fsm_evt_t evt, const void *data)
{
    switch (evt) {
    case FSM_EVT_STA_ASSOC: {
        const wifi_event_sta_connected_t *conn = data;
        size_t len = conn->ssid_len;
        if (len >= sizeof(s_connected_ssid)) len = sizeof(s_connected_ssid) - 1;

        char    log_ssid[WIFI_SSID_BUF_SIZE];
        uint8_t bssid[6];
        uint8_t channel;

        state_lock();
        memcpy(s_connected_ssid, conn->ssid, len);
        s_connected_ssid[len] = '\0';
        strncpy(s_connected_password, s_pending_password,
                sizeof(s_connected_password) - 1);
        s_connected_password[sizeof(s_connected_password) - 1] = '\0';
        memcpy(log_ssid, s_connected_ssid, sizeof(log_ssid));
        memcpy(bssid, conn->bssid, sizeof(bssid));
        channel = conn->channel;
        state_unlock();

        ESP_LOGI(TAG, "Associated with '%s' "
                 "(bssid=%02x:%02x:%02x:%02x:%02x:%02x, ch=%d)",
                 log_ssid, bssid[0], bssid[1], bssid[2],
                 bssid[3], bssid[4], bssid[5], channel);
        break;
    }
    case FSM_EVT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *disc = data;
        bool give_up   = false;
        bool voluntary = (disc->reason == WIFI_REASON_ASSOC_LEAVE);
        char log_ssid[WIFI_SSID_BUF_SIZE] = {0};

        state_lock();
        memcpy(log_ssid, s_connected_ssid, sizeof(log_ssid));
        atomic_store(&s_wifi_connected, false);
        s_connected_ssid[0] = '\0';
        s_connected_password[0] = '\0';

        bool has_target = false;
        if (s_pending_ssid[0]) {
            if (voluntary) {
                has_target = true;
            } else if (reason_is_fatal(disc->reason)) {
                s_pending_ssid[0] = '\0';
                give_up = true;
            } else if (s_reconnect_cnt < MAX_RECONNECT_ATTEMPTS) {
                s_reconnect_cnt++;
                has_target = true;
            } else {
                s_pending_ssid[0] = '\0';
                give_up = true;
            }
        }

        if (give_up) {
            fsm_set_state(WIFI_ST_FAILED);
        } else if (has_target) {
            fsm_set_state(voluntary ? WIFI_ST_CONNECTING : WIFI_ST_RECONNECTING);
        } else {
            fsm_set_state(WIFI_ST_IDLE);
        }
        state_unlock();

        ESP_LOGW(TAG, "DISCONNECTED, reason=%d, rssi=%d, ssid='%s'",
                 disc->reason, disc->rssi, log_ssid);

        if (s_events) xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);

        if (give_up) {
            atomic_store(&s_last_disconnect_reason, disc->reason);
            post_ui_event(MSG_WIFI_DISCONNECTED);
        } else if (has_target) {
            if (!voluntary) post_ui_event(MSG_WIFI_RECONNECTING);
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK)
                ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(ret));
        }
        break;
    }
    case FSM_EVT_GOT_IP:
        fsm_handle_got_ip(data);
        break;
    default:
        break;
    }
}

static void fsm_connected(fsm_evt_t evt, const void *data)
{
    switch (evt) {
    case FSM_EVT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *disc = data;
        char log_ssid[WIFI_SSID_BUF_SIZE] = {0};
        bool has_target;

        state_lock();
        memcpy(log_ssid, s_connected_ssid, sizeof(log_ssid));
        atomic_store(&s_wifi_connected, false);
        s_connected_ssid[0] = '\0';
        s_connected_password[0] = '\0';

        has_target = (s_pending_ssid[0] != '\0');
        if (has_target && reason_is_fatal(disc->reason)) {
            s_pending_ssid[0] = '\0';
            has_target = false;
        }
        if (has_target) {
            s_reconnect_cnt = 1;
            fsm_set_state(WIFI_ST_RECONNECTING);
        } else {
            fsm_set_state(WIFI_ST_IDLE);
        }
        state_unlock();

        ESP_LOGW(TAG, "Link lost, reason=%d, ssid='%s'",
                 disc->reason, log_ssid);

        if (s_events) xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);

        if (has_target) {
            post_ui_event(MSG_WIFI_RECONNECTING);
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK)
                ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(ret));
        } else {
            /* Nothing to reconnect to — terminal from the UI's view. */
            atomic_store(&s_last_disconnect_reason, disc->reason);
            post_ui_event(MSG_WIFI_DISCONNECTED);
        }
        break;
    }
    default:
        break;
    }
}

static void fsm_disconnecting(fsm_evt_t evt, const void *data)
{
    switch (evt) {
    case FSM_EVT_STA_DISCONNECTED: {
        /* voluntary disconnect completed; bring up the new target */
        bool has_target;
        state_lock();
        atomic_store(&s_wifi_connected, false);
        s_connected_ssid[0] = '\0';
        s_connected_password[0] = '\0';

        has_target = (s_pending_ssid[0] != '\0');
        if (has_target) {
            s_reconnect_cnt = 0;
            fsm_set_state(WIFI_ST_CONNECTING);
        } else {
            fsm_set_state(WIFI_ST_IDLE);
        }
        state_unlock();

        if (s_events) xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);

        if (has_target) {
            esp_err_t ret = esp_wifi_start();
            if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE)
                ESP_LOGW(TAG, "start: %s", esp_err_to_name(ret));

            ret = esp_wifi_connect();
            if (ret != ESP_OK)
                ESP_LOGW(TAG, "connect: %s", esp_err_to_name(ret));
        }
        break;
    }
    case FSM_EVT_GOT_IP:
        /* race: came back up before we noticed the disconnect */
        fsm_handle_got_ip(data);
        break;
    default:
        break;
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

static wifi_sntp_sync_cb_t s_sntp_sync_cb = NULL;

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

/* ---- Event handler ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_CONNECTED:
            fsm_dispatch(FSM_EVT_STA_ASSOC, event_data);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            fsm_dispatch(FSM_EVT_STA_DISCONNECTED, event_data);
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        fsm_dispatch(FSM_EVT_GOT_IP, event_data);
    }
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
    atomic_store(&s_last_disconnect_reason, 0);
    s_reconnect_cnt = 0;
    state_unlock();

    wifi_config_t config = {0};
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    strncpy((char *)config.sta.ssid,     ssid,     sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, password, sizeof(config.sta.password) - 1);

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    ret = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    ESP_LOGD(TAG, "esp_wifi_start -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) return ret;

    /* Already associated to some AP? Need a clean disconnect first,
       otherwise esp_wifi_connect() will no-op or race. */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "Already on '%s', disconnecting first", ap_info.ssid);
        state_lock();
        fsm_set_state(WIFI_ST_DISCONNECTING);
        state_unlock();
        post_ui_event(MSG_WIFI_DISCONNECTING);
        return esp_wifi_disconnect();
    }

    state_lock();
    fsm_set_state(WIFI_ST_CONNECTING);
    state_unlock();
    post_ui_event(MSG_WIFI_CONNECTING);

    ret = esp_wifi_connect();
    if (ret == ESP_ERR_WIFI_CONN) {
        ESP_LOGI(TAG, "Connect busy, forcing disconnect to switch target");
        state_lock();
        fsm_set_state(WIFI_ST_DISCONNECTING);
        state_unlock();
        post_ui_event(MSG_WIFI_DISCONNECTING);
        esp_wifi_disconnect();
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(ret));
        /* State stays CONNECTING — caller decides what to do. */
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
    atomic_store(&s_last_disconnect_reason, 0);
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

int32_t wifi_svc_get_last_disconnect_reason(void)
{
    return atomic_load(&s_last_disconnect_reason);
}

/* ---- Async: scan ---- */

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
    if (s_state == WIFI_ST_SCANNING) {
        fsm_set_state(WIFI_ST_IDLE);
    }
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
    if (s_state == WIFI_ST_SCANNING) {
        state_unlock();
        ESP_LOGW(TAG, "Scan already in progress, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    fsm_set_state(WIFI_ST_SCANNING);
    state_unlock();

    BaseType_t task_ret = xTaskCreate(scan_async_task, "wifi_svc_scan", WIFI_SVC_TASK_STACK, NULL, WIFI_SVC_TASK_PRIO, NULL);
    if (task_ret != pdPASS) {
        state_lock();
        if (s_state == WIFI_ST_SCANNING) {
            fsm_set_state(WIFI_ST_IDLE);
        }
        state_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Async: connect ---- */

typedef struct {
    char ssid[WIFI_SSID_BUF_SIZE];
    char password[WIFI_PASS_BUF_SIZE];
} connect_req_t;

static void connect_async_task(void *pv)
{
    connect_req_t *req = (connect_req_t *)pv;
    esp_err_t ret = wifi_svc_connect(req->ssid, req->password);
    free(req);

    if (ret != ESP_OK) {
        state_lock();
        if (s_state == WIFI_ST_CONNECTING || s_state == WIFI_ST_DISCONNECTING) {
            fsm_set_state(WIFI_ST_IDLE);
        }
        state_unlock();

        /* Sync failure: driver never reached association, so no
           STA_DISCONNECTED will arrive. UI is blocked in busy — we
           are the only ones who can unblock it. */
        atomic_store(&s_last_disconnect_reason, 0);    
        post_ui_event(MSG_WIFI_DISCONNECTED);
    }

    vTaskDelete(NULL);
}

esp_err_t wifi_svc_connect_async(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!password) password = "";

    esp_err_t ret = wifi_svc_init();
    if (ret != ESP_OK) return ret;

    state_lock();
    if (s_state == WIFI_ST_CONNECTING || s_state == WIFI_ST_DISCONNECTING) {
        state_unlock();
        ESP_LOGW(TAG, "Connect already in progress, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    fsm_set_state(WIFI_ST_CONNECTING);
    state_unlock();

    connect_req_t *req = malloc(sizeof(*req));
    if (!req) {
        state_lock();
        if (s_state == WIFI_ST_CONNECTING) {
            fsm_set_state(WIFI_ST_IDLE);
        }
        state_unlock();
        return ESP_ERR_NO_MEM;
    }

    strncpy(req->ssid, ssid, sizeof(req->ssid) - 1);
    req->ssid[sizeof(req->ssid) - 1] = '\0';
    strncpy(req->password, password, sizeof(req->password) - 1);
    req->password[sizeof(req->password) - 1] = '\0';

    BaseType_t task_ret = xTaskCreate(connect_async_task, "wifi_svc_conn", WIFI_SVC_TASK_STACK, req, WIFI_SVC_TASK_PRIO, NULL);
    if (task_ret != pdPASS) {
        free(req);
        state_lock();
        if (s_state == WIFI_ST_CONNECTING) {
            fsm_set_state(WIFI_ST_IDLE);
        }
        state_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}