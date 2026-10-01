#include "esp_timer.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "wifi_svc.h"
#include "bsp_display.h"
#include "esp_lvgl_port.h"  

#define TAG "UI"

/* ---- Sizes and angles ---- */
#define ARC_CO2_SIZE      300
#define ARC_CO2_WIDTH     16
#define ARC_TEMP_SIZE     160
#define ARC_TEMP_WIDTH    12
#define ARC_START_ANGLE   135
#define ARC_WIDTH_ANGLE   50
#define ARC_GAP_ANGLE     5

/* Placeholder shown in SSID dropdown before the first scan */
#define SSID_PLACEHOLDER  "Press Scan"

/* Buffer for dropdown options: N*ssid + (N-1) newlines + NUL + spare */
#define SCAN_OPTIONS_SIZE (WIFI_SCAN_MAX_RESULTS * WIFI_SSID_BUF_SIZE + 64)

/* ---- Fonts ---- */
LV_FONT_DECLARE(DroidSansMono_128);
LV_FONT_DECLARE(RobotoMono_88);
LV_FONT_DECLARE(RobotoMono_80);

/* ---- Palettes ---- */
static const uint32_t arc_co2_palette_hex[] = {
    0x006400, 0x7CFC00, 0xFFFF00, 0xFFA500, 0xFF0000
};
static const uint32_t arc_temp_hum_palette_hex[] = {
    0x4575B4, 0x74ADD1, 0xFEE090, 0xF46D43, 0xD73027
};

/* ---- Theme ---- */
static bool s_dark_theme = true;

static lv_obj_t *s_scr;
static lv_obj_t *s_tv;
static lv_obj_t *s_page_main;
static lv_obj_t *s_page_wifi;

/* ---- Main page widgets (clock + sensors) ---- */
static lv_obj_t *clock_label;

static lv_obj_t *label_txt_co2;
static lv_obj_t *label_txt_ppm;

static lv_obj_t *arc_co2[5];
static lv_obj_t *label_co2;

static lv_obj_t *arc_temp[5];
static lv_obj_t *label_temp;

static lv_obj_t *arc_hum[5];
static lv_obj_t *label_hum;

/* ---- Arc track styles ---- */
static lv_style_t style_arc_co2_bg;
static lv_style_t style_arc_temp_bg;
static lv_style_t style_arc_hum_bg;

/* ---- WiFi page widgets ---- */
static lv_obj_t *wifi_ssid_dropdown;
static lv_obj_t *wifi_password_textarea;
static lv_obj_t *wifi_config_status;
static lv_obj_t *wifi_keyboard;
static lv_obj_t *wifi_connect_button;
static lv_obj_t *wifi_scan_button;
static lv_obj_t *wifi_spinner;

/* ---- Shared widgets (visible on all pages) ---- */
static lv_obj_t *wifi_icon;
static lv_timer_t *wifi_blink_timer;
static bool wifi_blink_visible;

/* ---- Async WiFi event state ---- */
static volatile bool    s_wifi_event_pending;
static volatile int32_t s_wifi_event_id;

static void wifi_blink_cb(lv_timer_t *t)
{
    if (!wifi_icon) return;

    wifi_blink_visible = !wifi_blink_visible;
    if (wifi_blink_visible) {
        lv_obj_clear_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_icon_stop_blink(void)
{
    if (wifi_blink_timer) {
        lv_timer_delete(wifi_blink_timer);
        wifi_blink_timer = NULL;
    }
    wifi_blink_visible = true;
}

static void set_wifi_icon(bool connected, bool busy)
{
    if (!wifi_icon) return;

    wifi_icon_stop_blink();

    if (busy) {
        lv_obj_clear_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        wifi_blink_timer = lv_timer_create(wifi_blink_cb, 400, NULL);
    } else if (connected) {
        lv_obj_clear_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static int normalize_angle(int angle)
{
    if (angle > 360) angle = angle % 360;
    return angle;
}

static void arc_value_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)var, v);
    int32_t step = 25;
    int32_t discrete_v = (v / step) * step;
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)discrete_v);
    lv_label_set_text(label_co2, buf);
}

static void apply_theme(void)
{
    /* Reinit default theme with new dark/light flag */
    lv_theme_default_init(NULL,
                          lv_palette_main(LV_PALETTE_BLUE),
                          lv_palette_main(LV_PALETTE_ORANGE),
                          s_dark_theme,
                          LV_FONT_DEFAULT);

    /* Explicit colors not covered by theme */
    lv_color_t bg        = s_dark_theme ? lv_color_black() : lv_color_white();
    lv_color_t fg        = s_dark_theme ? lv_color_white() : lv_color_black();
    lv_color_t arc_track = s_dark_theme ? lv_color_hex(0x404040)
                                        : lv_color_hex(0xC0C0C0);

    if (s_scr) lv_obj_set_style_bg_color(s_scr, bg, 0);
    if (s_tv)  lv_obj_set_style_bg_color(s_tv,  bg, 0);

    /* Text colors cascade to children via inheritance */
    if (s_page_main) lv_obj_set_style_text_color(s_page_main, fg, 0);
    if (s_page_wifi) lv_obj_set_style_text_color(s_page_wifi, fg, 0);

    if (wifi_icon) lv_obj_set_style_text_color(wifi_icon, fg, 0);

    /* Arc tracks — update style; report_style_change will redraw */
    lv_style_set_arc_color(&style_arc_co2_bg,  arc_track);
    lv_style_set_arc_color(&style_arc_temp_bg, arc_track);
    lv_style_set_arc_color(&style_arc_hum_bg,  arc_track);

    /* Force all widgets to re-evaluate their styles */
    lv_obj_report_style_change(NULL);
}

static void theme_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_dark_theme = lv_obj_has_state(sw, LV_STATE_CHECKED);
    apply_theme();
}

static void animate_startup_indicators(void)
{
    lv_anim_t a;

    for (int i = 0; i < 5; i++) {
        lv_anim_init(&a);
        lv_anim_set_var(&a, arc_co2[i]);
        lv_anim_set_exec_cb(&a, arc_value_anim_cb);
        lv_anim_set_values(&a, 0, 2500);
        lv_anim_set_time(&a, 2500);
        lv_anim_set_delay(&a, 0);
        lv_anim_set_playback_time(&a, 2500);
        lv_anim_set_playback_delay(&a, 200);
        lv_anim_start(&a);
    }
}

void ui_update_clock(void)
{
    time_t now;
    
    time(&now);

    if (now < 1704067200) {   /* 2024-01-01 00:00:00 UTC */
        lv_label_set_text(clock_label, "--:--:--");
        return;
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
    lv_label_set_text(clock_label, time_str);
}

void ui_update_sensors(float co2, float temperature, float humidity)
{
    for (int i = 0; i < 5; i++) {
        lv_arc_set_value(arc_co2[i],  (int)co2);
        lv_arc_set_value(arc_temp[i], (int)(temperature * 10.0f));
        lv_arc_set_value(arc_hum[i],  (int)humidity);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", co2);
    lv_label_set_text(label_co2, buf);

    snprintf(buf, sizeof(buf), "%.1f", temperature);
    lv_label_set_text(label_temp, buf);

    snprintf(buf, sizeof(buf), "%.0f%%", humidity);
    lv_label_set_text(label_hum, buf);
}

static void update_connect_button_state(void)
{
    char ssid[WIFI_SSID_BUF_SIZE] = {0};
    lv_dropdown_get_selected_str(wifi_ssid_dropdown, ssid, sizeof(ssid));

    if (ssid[0] == '\0' || strcmp(ssid, SSID_PLACEHOLDER) == 0) {
        lv_obj_add_state(wifi_connect_button, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(wifi_connect_button, LV_STATE_DISABLED);
    }
}

static void set_wifi_inputs_busy(bool busy)
{
    int64_t t0 = esp_timer_get_time();
    if (busy) {
        lv_obj_add_state(wifi_ssid_dropdown,     LV_STATE_DISABLED);
        lv_obj_add_state(wifi_password_textarea, LV_STATE_DISABLED);
        lv_obj_add_state(wifi_scan_button,       LV_STATE_DISABLED);
        lv_obj_add_state(wifi_connect_button,    LV_STATE_DISABLED);
        ESP_LOGI(TAG, "block: %lld us", esp_timer_get_time() - t0);
    } else {
        lv_obj_clear_state(wifi_ssid_dropdown,     LV_STATE_DISABLED);
        lv_obj_clear_state(wifi_password_textarea, LV_STATE_DISABLED);
        lv_obj_clear_state(wifi_scan_button,       LV_STATE_DISABLED);
        update_connect_button_state();
        ESP_LOGI(TAG, "unblock: %lld us", esp_timer_get_time() - t0);
    }
}

static void wifi_dropdown_value_changed_cb(lv_event_t *event)
{
    (void)event;
    char ssid[WIFI_SSID_BUF_SIZE] = {0};
    lv_dropdown_get_selected_str(wifi_ssid_dropdown, ssid, sizeof(ssid));

    char dummy_password[WIFI_PASS_BUF_SIZE] = {0};
    if (wifi_svc_get_saved_password(ssid, dummy_password, sizeof(dummy_password))) {
        lv_textarea_set_text(wifi_password_textarea, dummy_password);
        lv_label_set_text(wifi_config_status, "Network is saved in memory");

        lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_send_event(wifi_password_textarea, LV_EVENT_DEFOCUSED, NULL);
    } else {
        lv_textarea_set_text(wifi_password_textarea, "");
        lv_label_set_text(wifi_config_status, "Enter password to connect");

        lv_obj_clear_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_send_event(wifi_password_textarea, LV_EVENT_FOCUSED, NULL);
    }

    update_connect_button_state();
}

static void wifi_scan_event_cb(lv_event_t *event)
{
    (void)event;

    set_wifi_inputs_busy(true);
    lv_obj_clear_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);

    esp_err_t ret = wifi_svc_scan_async();
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            lv_label_set_text(wifi_config_status, "Scan already in progress");
        } else {
            lv_label_set_text(wifi_config_status, "Failed to start scan");
            ESP_LOGE(TAG, "wifi_svc_scan_async: %s", esp_err_to_name(ret));
        }
        set_wifi_inputs_busy(false);
        lv_obj_add_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_connect_event_cb(lv_event_t *event)
{
    (void)event;
    char ssid[WIFI_SSID_BUF_SIZE] = {0};
    char password[WIFI_PASS_BUF_SIZE] = {0};

    lv_dropdown_get_selected_str(wifi_ssid_dropdown, ssid, sizeof(ssid));

    /* Safety net: button should be disabled, but guard anyway. */
    if (ssid[0] == '\0' || strcmp(ssid, SSID_PLACEHOLDER) == 0) {
        lv_label_set_text(wifi_config_status, "Scan first, then select a network");
        return;
    }

    strncpy(password, lv_textarea_get_text(wifi_password_textarea),
            sizeof(password) - 1);

    lv_label_set_text(wifi_config_status, "Starting connection...");
    
    set_wifi_inputs_busy(true);

    esp_err_t ret = wifi_svc_connect_async(ssid, password);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            lv_label_set_text(wifi_config_status, "Already connecting...");
        } else {
            lv_label_set_text(wifi_config_status, "Failed to start connect");
        }
        set_wifi_inputs_busy(false);
    }
}

static void toggle_password_visibility_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *ta  = (lv_obj_t *)lv_event_get_user_data(e);
    bool password_mode = lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(ta, !password_mode);

    lv_obj_t *label = lv_obj_get_child(btn, 0);
    lv_label_set_text(label, password_mode ? LV_SYMBOL_EYE_CLOSE
                                            : LV_SYMBOL_EYE_OPEN);
    lv_obj_invalidate(lv_screen_active());
}

static void set_scan_results(const char ssids[][WIFI_SSID_BUF_SIZE], size_t count)
{
    static char options[SCAN_OPTIONS_SIZE]; 
    options[0] = '\0';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) strncat(options, "\n", sizeof(options) - strlen(options) - 1);
        strncat(options, ssids[i], sizeof(options) - strlen(options) - 1);
    }

    lv_dropdown_set_options(wifi_ssid_dropdown, count ? options : "No networks found");
    lv_label_set_text_fmt(wifi_config_status, "%u networks found", (unsigned)count);

    if (count > 0) {
        lv_obj_send_event(wifi_ssid_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

void ui_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    s_wifi_event_id = id;
    s_wifi_event_pending = true;
}

static void wifi_apply_cb(lv_timer_t *t)
{
    (void)t;

    if (!s_wifi_event_pending) return;
    s_wifi_event_pending = false;

    int32_t id = s_wifi_event_id;
    char ssid[WIFI_SSID_BUF_SIZE];

    switch (id) {
    case MSG_WIFI_SCANNING:
        set_wifi_icon(false, true);
        lv_label_set_text(wifi_config_status, "Scanning...");
        set_wifi_inputs_busy(true);
        lv_obj_clear_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);
        break;

    case MSG_WIFI_CONNECTING:
        set_wifi_icon(false, true);
        if (wifi_svc_get_target_ssid(ssid, sizeof(ssid)) == ESP_OK) {
            lv_label_set_text_fmt(wifi_config_status, "Connecting to: %s…", ssid);
        } else {
            lv_label_set_text(wifi_config_status, "Connecting…");
        }
        set_wifi_inputs_busy(true);
        break;

    case MSG_WIFI_CONNECTED:
        set_wifi_icon(true, false);
        if (wifi_svc_get_current_ssid(ssid, sizeof(ssid)) == ESP_OK) {
            lv_label_set_text_fmt(wifi_config_status, "Connected to: %s", ssid);
        } else {
            lv_label_set_text(wifi_config_status, "Connected");
        }
        set_wifi_inputs_busy(false);
        lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        break;

    case MSG_WIFI_DISCONNECTED:
        set_wifi_icon(false, false);
        lv_label_set_text(wifi_config_status, "Connection failed (Wrong password?)");
        set_wifi_inputs_busy(false);
        break;

    case MSG_WIFI_SCAN_SUCCESS: {
        static char ssids[WIFI_SCAN_MAX_RESULTS][WIFI_SSID_BUF_SIZE];
        size_t n = wifi_svc_scan_get_results(ssids, WIFI_SCAN_MAX_RESULTS);
        set_scan_results(ssids, n);
        set_wifi_icon(wifi_svc_is_connected(), false);
        set_wifi_inputs_busy(false);
        lv_obj_add_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    case MSG_WIFI_SCAN_FAILED:
        lv_label_set_text(wifi_config_status, "WiFi scan failed");
        set_wifi_icon(wifi_svc_is_connected(), false);
        set_wifi_inputs_busy(false);
        lv_obj_add_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);
        break;

    default:
        break;
    }
}

static void create_main_page(lv_obj_t *parent);
static void create_wifi_page(lv_obj_t *parent);

void ui_create(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "ui_create: failed to take LVGL lock");
        return;
    }

    /* Initial theme setup — will be re-applied at the end */
    lv_theme_default_init(NULL,
                          lv_palette_main(LV_PALETTE_BLUE),
                          lv_palette_main(LV_PALETTE_ORANGE),
                          s_dark_theme,
                          LV_FONT_DEFAULT);

    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);

    s_tv = lv_tileview_create(s_scr);
    lv_obj_add_flag(s_tv, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_size(s_tv, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_tv);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);

    lv_obj_set_style_pad_all(s_tv, 0, 0);
    lv_obj_set_style_border_width(s_tv, 0, 0);
    lv_obj_set_scroll_dir(s_tv, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_tv, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scroll_snap_y(s_tv, LV_SCROLL_SNAP_NONE);

    s_page_main = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_page_wifi = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT);

    lv_obj_clear_flag(s_page_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_page_wifi, LV_OBJ_FLAG_SCROLLABLE);

    create_main_page(s_page_main);
    create_wifi_page(s_page_wifi);

    /* WiFi icon on top layer */
    wifi_icon = lv_label_create(lv_layer_top());
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_22, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_RIGHT, -12, 8);
    set_wifi_icon(false, false);

    apply_theme();
    lv_timer_create(wifi_apply_cb, 100, NULL);
    ui_update_clock();
    lvgl_port_unlock();
}

static void create_main_page(lv_obj_t *parent)
{
    /* --- Clock --- */
    clock_label = lv_label_create(parent);
    lv_obj_set_style_text_font(clock_label, &DroidSansMono_128, 0);
    lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, 48);
    lv_label_set_text(clock_label, "--:--:--");

    /* --- CO2 label + value --- */
    label_txt_co2 = lv_label_create(parent);
    lv_obj_set_style_text_font(label_txt_co2, &RobotoMono_80, 0);
    lv_obj_align(label_txt_co2, LV_ALIGN_CENTER, 0, 32);
    lv_label_set_text(label_txt_co2, "CO2");

    label_txt_ppm = lv_label_create(parent);
    lv_obj_set_style_text_font(label_txt_ppm, &RobotoMono_80, 0);
    lv_obj_align(label_txt_ppm, LV_ALIGN_CENTER, 0, 160);
    lv_label_set_text(label_txt_ppm, "ppm");

    label_co2 = lv_label_create(parent);
    lv_obj_set_style_text_font(label_co2, &RobotoMono_88, 0);
    lv_obj_align(label_co2, LV_ALIGN_CENTER, 0, 96);
    lv_label_set_text(label_co2, "0000");

    /* --- CO2 arcs --- */
    lv_style_init(&style_arc_co2_bg);
    lv_style_set_arc_width(&style_arc_co2_bg, ARC_CO2_WIDTH);
    lv_style_set_arc_rounded(&style_arc_co2_bg, false);
    lv_style_set_arc_color(&style_arc_co2_bg, lv_color_hex(0x404040));

    for (int i = 0; i < 5; i++) {
        arc_co2[i] = lv_arc_create(parent);
        lv_obj_set_size(arc_co2[i], ARC_CO2_SIZE, ARC_CO2_SIZE);
        lv_obj_align(arc_co2[i], LV_ALIGN_CENTER, 0, 96);

        lv_obj_remove_style(arc_co2[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc_co2[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_arc_color(arc_co2[i], lv_color_hex(arc_co2_palette_hex[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc_co2[i], ARC_CO2_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc_co2[i], false, LV_PART_INDICATOR);
        lv_obj_add_style(arc_co2[i], &style_arc_co2_bg, LV_PART_MAIN);

        lv_arc_set_range(arc_co2[i], 0 + i * 500, 500 + i * 500);

        int arc_start_angle = ARC_START_ANGLE + i * (ARC_WIDTH_ANGLE + ARC_GAP_ANGLE);
        int arc_end_angle   = arc_start_angle + ARC_WIDTH_ANGLE;
        lv_arc_set_bg_start_angle(arc_co2[i], normalize_angle(arc_start_angle));
        lv_arc_set_bg_end_angle(arc_co2[i],   normalize_angle(arc_end_angle));
        lv_arc_set_mode(arc_co2[i], LV_ARC_MODE_NORMAL);
    }

    /* --- Temperature --- */
    label_temp = lv_label_create(parent);
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_48, 0);
    lv_obj_align(label_temp, LV_ALIGN_CENTER, -264, 160);
    lv_label_set_text(label_temp, "00.0");

    lv_style_init(&style_arc_temp_bg);
    lv_style_set_arc_width(&style_arc_temp_bg, ARC_TEMP_WIDTH);
    lv_style_set_arc_rounded(&style_arc_temp_bg, false);
    lv_style_set_arc_color(&style_arc_temp_bg, lv_color_hex(0x404040));

    for (int i = 0; i < 5; i++) {
        arc_temp[i] = lv_arc_create(parent);
        lv_obj_set_size(arc_temp[i], ARC_TEMP_SIZE, ARC_TEMP_SIZE);
        lv_obj_align(arc_temp[i], LV_ALIGN_CENTER, -264, 160);

        lv_obj_remove_style(arc_temp[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc_temp[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_arc_color(arc_temp[i], lv_color_hex(arc_temp_hum_palette_hex[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc_temp[i], ARC_TEMP_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc_temp[i], false, LV_PART_INDICATOR);
        lv_obj_add_style(arc_temp[i], &style_arc_temp_bg, LV_PART_MAIN);

        lv_arc_set_range(arc_temp[i], 225 + i * 10, 235 + i * 10);

        int arc_start_angle = ARC_START_ANGLE + i * (ARC_WIDTH_ANGLE + ARC_GAP_ANGLE);
        int arc_end_angle   = arc_start_angle + ARC_WIDTH_ANGLE;
        lv_arc_set_bg_start_angle(arc_temp[i], normalize_angle(arc_start_angle));
        lv_arc_set_bg_end_angle(arc_temp[i],   normalize_angle(arc_end_angle));
        lv_arc_set_mode(arc_temp[i], LV_ARC_MODE_NORMAL);
    }

    /* --- Humidity --- */
    label_hum = lv_label_create(parent);
    lv_obj_set_style_text_font(label_hum, &lv_font_montserrat_48, 0);
    lv_obj_align(label_hum, LV_ALIGN_CENTER, 264, 160);
    lv_label_set_text(label_hum, "00");

    lv_style_init(&style_arc_hum_bg);
    lv_style_set_arc_width(&style_arc_hum_bg, ARC_TEMP_WIDTH);
    lv_style_set_arc_rounded(&style_arc_hum_bg, false);
    lv_style_set_arc_color(&style_arc_hum_bg, lv_color_hex(0x404040));

    for (int i = 0; i < 5; i++) {
        arc_hum[i] = lv_arc_create(parent);
        lv_obj_set_size(arc_hum[i], ARC_TEMP_SIZE, ARC_TEMP_SIZE);
        lv_obj_align(arc_hum[i], LV_ALIGN_CENTER, 264, 160);

        lv_obj_remove_style(arc_hum[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc_hum[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_arc_color(arc_hum[i], lv_color_hex(arc_temp_hum_palette_hex[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc_hum[i], ARC_TEMP_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc_hum[i], false, LV_PART_INDICATOR);
        lv_obj_add_style(arc_hum[i], &style_arc_hum_bg, LV_PART_MAIN);

        lv_arc_set_range(arc_hum[i], 0 + i * 20, 20 + i * 20);

        int arc_start_angle = ARC_START_ANGLE + i * (ARC_WIDTH_ANGLE + ARC_GAP_ANGLE);
        int arc_end_angle   = arc_start_angle + ARC_WIDTH_ANGLE;
        lv_arc_set_bg_start_angle(arc_hum[i], normalize_angle(arc_start_angle));
        lv_arc_set_bg_end_angle(arc_hum[i],   normalize_angle(arc_end_angle));
        lv_arc_set_mode(arc_hum[i], LV_ARC_MODE_NORMAL);
    }

    animate_startup_indicators();
}

static void create_wifi_page(lv_obj_t *parent)
{
    /* --- Title --- */
    lv_obj_t *wifi_title = lv_label_create(parent);
    lv_label_set_text(wifi_title, "WiFi settings");
    lv_obj_align(wifi_title, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_text_font(wifi_title, &lv_font_montserrat_22, LV_PART_MAIN);

    /* --- Theme toggle (top-right) --- */
    lv_obj_t *theme_label = lv_label_create(parent);
    lv_label_set_text(theme_label, "Dark");
    lv_obj_set_style_text_font(theme_label, &lv_font_montserrat_20, 0);
    lv_obj_align(theme_label, LV_ALIGN_TOP_RIGHT, -72, 22);

    lv_obj_t *theme_switch = lv_switch_create(parent);
    lv_obj_align(theme_switch, LV_ALIGN_TOP_RIGHT, -16, 16);
    if (s_dark_theme) lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(theme_switch, theme_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- SSID dropdown --- */
    wifi_ssid_dropdown = lv_dropdown_create(parent);
    lv_obj_set_width(wifi_ssid_dropdown, 312);
    lv_obj_align(wifi_ssid_dropdown, LV_ALIGN_TOP_MID, -200, 48);
    lv_dropdown_set_options(wifi_ssid_dropdown, SSID_PLACEHOLDER);
    lv_obj_set_style_text_font(wifi_ssid_dropdown, &lv_font_montserrat_22, LV_PART_MAIN);

    lv_obj_t *list = lv_dropdown_get_list(wifi_ssid_dropdown);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_22, LV_PART_MAIN);

    /* --- Scan button --- */
    wifi_scan_button = lv_button_create(parent);
    lv_obj_set_size(wifi_scan_button, 128, 48);
    lv_obj_align(wifi_scan_button, LV_ALIGN_TOP_MID, -200, 128);
    lv_obj_t *scan_label = lv_label_create(wifi_scan_button);
    lv_label_set_text(scan_label, "Scan");
    lv_obj_center(scan_label);
    lv_obj_set_style_text_font(wifi_scan_button, &lv_font_montserrat_22, LV_PART_MAIN);

    /* Spinner next to Scan — shown while scanning */
    wifi_spinner = lv_spinner_create(parent);
    lv_obj_set_size(wifi_spinner, 24, 24);
    lv_obj_align(wifi_spinner, LV_ALIGN_TOP_MID, -96, 140);
    lv_obj_set_style_arc_width(wifi_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(wifi_spinner, 3, LV_PART_INDICATOR);
    lv_obj_add_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);

    /* --- Password textarea --- */
    wifi_password_textarea = lv_textarea_create(parent);
    lv_obj_set_size(wifi_password_textarea, 312, 48);
    lv_obj_align(wifi_password_textarea, LV_ALIGN_TOP_MID, 200, 48);
    lv_textarea_set_placeholder_text(wifi_password_textarea, "WiFi password");
    lv_textarea_set_password_mode(wifi_password_textarea, true);
    lv_obj_set_style_text_font(wifi_password_textarea, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(wifi_password_textarea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(wifi_password_textarea, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(wifi_password_textarea, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(wifi_password_textarea, 45, LV_PART_MAIN);

    /* --- Password visibility toggle --- */
    lv_obj_t *password_btn = lv_button_create(parent);
    lv_obj_set_size(password_btn, 48, 48);
    lv_obj_align_to(password_btn, wifi_password_textarea, LV_ALIGN_OUT_RIGHT_MID, -48, 0);
    lv_obj_set_style_bg_opa(password_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(password_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(password_btn, 0, 0);

    lv_obj_t *btn_label = lv_label_create(password_btn);
    lv_label_set_text(btn_label, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(btn_label);
    lv_obj_set_style_text_font(password_btn, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(password_btn, lv_palette_main(LV_PALETTE_GREY), 0);

    /* --- On-screen keyboard --- */
    wifi_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(wifi_keyboard, 762, 256);
    lv_obj_align(wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_font(wifi_keyboard, &lv_font_montserrat_22, LV_PART_ITEMS);
    lv_keyboard_set_textarea(wifi_keyboard, wifi_password_textarea);
    lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);

    /* --- Connect button --- */
    wifi_connect_button = lv_button_create(parent);
    lv_obj_set_size(wifi_connect_button, 128, 48);
    lv_obj_align(wifi_connect_button, LV_ALIGN_TOP_MID, 200, 128);
    lv_obj_t *connect_label = lv_label_create(wifi_connect_button);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_add_state(wifi_connect_button, LV_STATE_DISABLED);

    /* --- Status label --- */
    wifi_config_status = lv_label_create(parent);
    lv_obj_align(wifi_config_status, LV_ALIGN_TOP_MID, 0, 184);
    lv_obj_set_width(wifi_config_status, 400);
    lv_label_set_long_mode(wifi_config_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(wifi_config_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wifi_config_status, &lv_font_montserrat_20, 0);
    lv_label_set_text(wifi_config_status, "Select a network");

    /* --- Event callbacks --- */
    lv_obj_add_event_cb(wifi_ssid_dropdown, wifi_dropdown_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(wifi_scan_button, wifi_scan_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(wifi_connect_button, wifi_connect_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(password_btn, toggle_password_visibility_cb, LV_EVENT_CLICKED, wifi_password_textarea);
}