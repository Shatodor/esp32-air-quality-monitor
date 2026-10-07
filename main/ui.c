#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "wifi_svc.h"
#include "esp_lvgl_port.h"  

#define TAG "UI"

/* ---- Main page layout ----
 * Three columns: temp | co2 | hum. Each column aligns its label
 * and its arcs to the same X. */
#define COL_HUM_X       264
#define COL_TEMP_X      (-COL_HUM_X)
#define COL_CO2_X       0


/* Y positions inside the central CO2 column */
#define CO2_TITLE_Y     32
#define CO2_VALUE_Y     96
#define CO2_UNITS_Y     160

/* Y of the temp/hum label row and their arcs */
#define COL_TEMP_HUM_Y  160

/* Clock */
#define CLOCK_X         0
#define CLOCK_Y         48

/* ---- WiFi page layout ---- */
#define WIFI_RIGHT_X    200
#define WIFI_LEFT_X     (-WIFI_RIGHT_X)

/* ---- Animation ---- */
#define CO2_ANIM_QUANTUM 25   /* round animated digits to this step */
#define ARC_GROUP_COUNT  3    /* co2, temp, hum */

/* ---- Timing ---- */
#define WIFI_EVENT_POLL_MS  100
#define WIFI_BLINK_MS       400

/* ---- Sizes and angles ---- */
#define ARC_CO2_SIZE      300
#define ARC_CO2_WIDTH     16
#define ARC_TEMP_SIZE     160
#define ARC_TEMP_WIDTH    12
#define ARC_START_ANGLE   135
#define ARC_WIDTH_ANGLE   50
#define ARC_GAP_ANGLE     5

#define ARC_SEGMENT_COUNT 5

#define CO2_ARC_MIN       0
#define CO2_ARC_STEP      500
#define CO2_ARC_MAX       (CO2_ARC_MIN + ARC_SEGMENT_COUNT * CO2_ARC_STEP)   /* 2500 */

#define TEMP_ARC_MIN      200   /* tenths of a degree: 20.0 C */
#define TEMP_ARC_STEP     20
#define TEMP_ARC_MAX      (TEMP_ARC_MIN + ARC_SEGMENT_COUNT * TEMP_ARC_STEP)  /* 275 */

#define HUM_ARC_MIN       0
#define HUM_ARC_STEP      20
#define HUM_ARC_MAX       (HUM_ARC_MIN + ARC_SEGMENT_COUNT * HUM_ARC_STEP)    /* 100 */

/* Placeholder shown in SSID dropdown before the first scan */
#define SSID_PLACEHOLDER  "Press Scan"
#define SSID_NO_RESULTS   "No networks found"

/* Buffer for dropdown options: N*ssid + (N-1) newlines + NUL + spare */
#define SCAN_OPTIONS_SIZE (WIFI_SCAN_MAX_RESULTS * WIFI_SSID_BUF_SIZE + 64)

/* ---- Fonts ---- */
LV_FONT_DECLARE(DroidSansMono_128);
LV_FONT_DECLARE(RobotoMono_88);
LV_FONT_DECLARE(RobotoMono_80);

/* ---- Palettes ---- */
static const uint32_t arc_co2_palette_hex[ARC_SEGMENT_COUNT] = {
    0x006400, 0x7CFC00, 0xFFFF00, 0xFFA500, 0xFF0000
};
static const uint32_t arc_temp_hum_palette_hex[ARC_SEGMENT_COUNT] = {
    0x4575B4, 0x74ADD1, 0xFEE090, 0xF46D43, 0xD73027
};

/* ---- Theme ---- */
static bool s_dark_theme = true;

static lv_obj_t *s_scr;
static lv_obj_t *s_tv;
static lv_obj_t *s_page_main;
static lv_obj_t *s_page_wifi;

typedef struct {
    int32_t co2;    /* ppm */
    int32_t temp;   /* tenths of a degree — matches arc units */
    int32_t hum;    /* % */
    bool    valid;  /* true once at least one measurement arrived */
} anim_target_t;

typedef enum {
    ANIM_OFF,       /* not running; ui_update_sensors acts directly */
    ANIM_FORWARD,   /* forward sweep in progress */
    ANIM_WAIT,      /* forward done, waiting for first measurement */
    ANIM_RETURN,    /* return sweep in progress */
} anim_phase_t;

static anim_target_t s_anim_target;
static anim_phase_t  s_anim_phase   = ANIM_OFF;
static int           s_anim_pending = 0;

/* ---- Main page widgets (clock + sensors) ---- */
static lv_obj_t *clock_label;

static lv_obj_t *label_txt_co2;
static lv_obj_t *label_txt_ppm;

static lv_obj_t *arc_co2[ARC_SEGMENT_COUNT];
static lv_obj_t *label_co2;

static lv_obj_t *arc_temp[ARC_SEGMENT_COUNT];
static lv_obj_t *label_temp;

static lv_obj_t *arc_hum[ARC_SEGMENT_COUNT];
static lv_obj_t *label_hum;

/* ---- Arc track styles ---- */
static lv_style_t style_arc_co2_bg;
static lv_style_t style_arc_temp_bg;
static lv_style_t style_arc_hum_bg;

/* ---- WiFi page widgets ---- */
static lv_obj_t *wifi_ssid_dropdown;
static lv_obj_t *wifi_password_textarea;
static lv_obj_t *wifi_status_label;
static lv_obj_t *wifi_keyboard;
static lv_obj_t *wifi_connect_button;
static lv_obj_t *wifi_forget_button;
static lv_obj_t *wifi_scan_button;
static lv_obj_t *wifi_spinner;

static bool s_wifi_scanned; 

/* ---- Shared widgets (visible on all pages) ---- */
static lv_obj_t *wifi_icon;
static lv_timer_t *wifi_blink_timer;
static bool wifi_blink_visible;

/* ---- Async WiFi event state ---- */
static volatile bool    s_wifi_event_pending;
static volatile int32_t s_wifi_event_id;

/* ---- Forward declarations ---- */
static void create_main_page(lv_obj_t *parent);
static void create_wifi_page(lv_obj_t *parent);
static void anim_start_return(void);

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
        wifi_blink_timer = lv_timer_create(wifi_blink_cb, WIFI_BLINK_MS, NULL);
    } else if (connected) {
        lv_obj_clear_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static int angle_mod360(int angle)
{
    if (angle > 360) angle = angle % 360;
    return angle;
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

static void open_settings_cb(lv_event_t *e)
{
    (void)e;
    lv_tileview_set_tile(s_tv, s_page_wifi, LV_ANIM_OFF);
}

static void close_settings_cb(lv_event_t *e)
{
    (void)e;
    lv_tileview_set_tile(s_tv, s_page_main, LV_ANIM_OFF);
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
    s_anim_target.co2  = (int32_t)co2;
    s_anim_target.temp = (int32_t)(temperature * 10.0f);
    s_anim_target.hum  = (int32_t)humidity;
    s_anim_target.valid = true;

    /* Forward done, waiting for the first measurement — this is it. */
    if (s_anim_phase == ANIM_WAIT && s_anim_target.valid) {
        anim_start_return();
    }

    if (s_anim_phase != ANIM_OFF) return;

    for (int i = 0; i < ARC_SEGMENT_COUNT; i++) {
        lv_arc_set_value(arc_co2[i],  s_anim_target.co2);
        lv_arc_set_value(arc_temp[i], s_anim_target.temp);
        lv_arc_set_value(arc_hum[i],  s_anim_target.hum);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f",   co2);         lv_label_set_text(label_co2,  buf);
    snprintf(buf, sizeof(buf), "%.1f",   temperature); lv_label_set_text(label_temp, buf);
    snprintf(buf, sizeof(buf), "%.0f%%", humidity);    lv_label_set_text(label_hum,  buf);
}

/* ============================================================
 * Startup animation
 * ============================================================ */

/* ---- exec callbacks: set arc value + label ---- */

static void anim_co2_exec(void *arc, int32_t v)
{
    lv_arc_set_value(arc, v);
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)(v / CO2_ANIM_QUANTUM * CO2_ANIM_QUANTUM));
    lv_label_set_text(label_co2, buf);
}

static void anim_temp_exec(void *arc, int32_t v)
{
    lv_arc_set_value(arc, v);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", v / 10.0f);
    lv_label_set_text(label_temp, buf);
}

static void anim_hum_exec(void *arc, int32_t v)
{
    lv_arc_set_value(arc, v);
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld%%", (long)v);
    lv_label_set_text(label_hum, buf);
}

/* ---- Completion callbacks ---- */

static void anim_return_done(lv_anim_t *a)
{
    (void)a;
    if (--s_anim_pending == 0) {
        s_anim_phase = ANIM_OFF;
    }
}

static void anim_forward_done(lv_anim_t *a)
{
    (void)a;
    if (--s_anim_pending == 0) {
        if (s_anim_target.valid) {
            anim_start_return();
        } else {
            s_anim_phase = ANIM_WAIT;
        }
    }
}

/* ---- Starters ---- */

static void anim_start_return_one(lv_obj_t *arc, lv_anim_exec_xcb_t exec, int32_t target, int32_t group_max)
{
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, arc);
    lv_anim_set_exec_cb(&b, exec);
    lv_anim_set_values(&b, group_max, target);
    lv_anim_set_time(&b, 2200);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&b, anim_return_done);
    lv_anim_start(&b);
}

static void anim_start_return(void)
{
    s_anim_phase   = ANIM_RETURN;
    s_anim_pending = ARC_GROUP_COUNT * ARC_SEGMENT_COUNT;

    for (int i = 0; i < ARC_SEGMENT_COUNT; i++) {
        anim_start_return_one(arc_co2[i],  anim_co2_exec,  s_anim_target.co2,  CO2_ARC_MAX);
        anim_start_return_one(arc_temp[i], anim_temp_exec, s_anim_target.temp, TEMP_ARC_MAX);
        anim_start_return_one(arc_hum[i],  anim_hum_exec,  s_anim_target.hum,  HUM_ARC_MAX);
    }
}

static void anim_start_group(lv_obj_t **arcs, lv_anim_exec_xcb_t exec,  int32_t lo, int32_t hi)
{
    for (int i = 0; i < ARC_SEGMENT_COUNT; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, arcs[i]);
        lv_anim_set_exec_cb(&a, exec);
        lv_anim_set_values(&a, lo, hi);
        lv_anim_set_time(&a, 6000);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, anim_forward_done);
        lv_anim_start(&a);
    }
}

/* ---- entry point (called from create_main_page) ---- */

static void anim_startup(void)
{
    s_anim_phase   = ANIM_FORWARD;
    s_anim_pending = ARC_GROUP_COUNT * ARC_SEGMENT_COUNT;

    anim_start_group(arc_co2,  anim_co2_exec,  CO2_ARC_MIN,  CO2_ARC_MAX);
    anim_start_group(arc_temp, anim_temp_exec, TEMP_ARC_MIN, TEMP_ARC_MAX);
    anim_start_group(arc_hum,  anim_hum_exec,  HUM_ARC_MIN,  HUM_ARC_MAX);
}

static void update_connect_button_state(void)
{
    char ssid[WIFI_SSID_BUF_SIZE] = {0};
    lv_dropdown_get_selected_str(wifi_ssid_dropdown, ssid, sizeof(ssid));

    if (!s_wifi_scanned ||
        ssid[0] == '\0' ||
        strcmp(ssid, SSID_PLACEHOLDER) == 0 ||
        strcmp(ssid, SSID_NO_RESULTS) == 0) {
        lv_obj_add_state(wifi_connect_button, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(wifi_connect_button, LV_STATE_DISABLED);
    }
}

static const char *disconnect_reason_str(int32_t reason)
{
    switch (reason) {
    /* Wrong password / auth rejected — IDF reports any of these
       depending on the phase where the handshake died. */
    case 15:   /* WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT */
    case 202:  /* WIFI_REASON_AUTH_FAIL */
    case 204:  /* WIFI_REASON_HANDSHAKE_TIMEOUT */
    case 205:  /* WIFI_REASON_CONNECTION_FAIL */
        return "Wrong password";

    case 201:  /* WIFI_REASON_NO_AP_FOUND */
        return "Network not found";

    case 203:  /* WIFI_REASON_ASSOC_FAIL */
        return "Association failed";

    case 200:  /* WIFI_REASON_BEACON_TIMEOUT */
        return "Connection lost";

    case 0:
        return "Connect failed";

    default:
        return "Disconnected";
    }
}

static void wifi_password_defocus(void)
{
    lv_group_t *g = lv_obj_get_group(wifi_password_textarea);
    if (g) lv_group_remove_obj(wifi_password_textarea);

    if (lv_obj_has_state(wifi_password_textarea, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY)) {
        lv_obj_remove_state(wifi_password_textarea, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        lv_obj_send_event(wifi_password_textarea, LV_EVENT_DEFOCUSED, NULL);
    }
}

/* Enable/disable the password field. Disabling implies defocus —
   a disabled textarea must never show a blinking cursor. */
static void wifi_password_enable(bool enable)
{
    if (enable) {
        lv_obj_clear_state(wifi_password_textarea, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(wifi_password_textarea, LV_STATE_DISABLED);
        wifi_password_defocus();
    }
}

/* Focus the field. Requires the field to be editable, so it is
   enabled first — a disabled textarea must not be focused. */
static void wifi_password_focus(void)
{
    wifi_password_enable(true);   
    lv_group_t *g = lv_obj_get_group(wifi_keyboard);
    if (!g) {
        ESP_LOGW(TAG, "keyboard has no group; cannot focus textarea");
        return;
    }
    lv_group_add_obj(g, wifi_password_textarea);
    lv_obj_add_state(wifi_password_textarea, LV_STATE_FOCUSED);
    lv_obj_send_event(wifi_password_textarea, LV_EVENT_FOCUSED, NULL);
}

static void set_wifi_inputs_busy(bool busy)
{
    if (busy) {
        lv_obj_add_state(wifi_ssid_dropdown,  LV_STATE_DISABLED);
        lv_obj_add_state(wifi_scan_button,    LV_STATE_DISABLED);
        lv_obj_add_state(wifi_connect_button, LV_STATE_DISABLED);
        lv_obj_add_state(wifi_forget_button,  LV_STATE_DISABLED);
        wifi_password_enable(false);
    } else {
        lv_obj_clear_state(wifi_scan_button,   LV_STATE_DISABLED);
        lv_obj_clear_state(wifi_forget_button, LV_STATE_DISABLED);

        if (s_wifi_scanned) {
            lv_obj_clear_state(wifi_ssid_dropdown, LV_STATE_DISABLED);
            wifi_password_enable(true);
        }
        update_connect_button_state();
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
        lv_label_set_text(wifi_status_label, "Network is saved in memory");

        lv_obj_clear_flag(wifi_forget_button, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        wifi_password_defocus();
    } else {
        lv_textarea_set_text(wifi_password_textarea, "");
        lv_label_set_text(wifi_status_label, "Enter password to connect");

        lv_obj_add_flag(wifi_forget_button, LV_OBJ_FLAG_HIDDEN);

        if (!lv_obj_has_state(wifi_password_textarea, LV_STATE_DISABLED)) {
            lv_obj_clear_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
            wifi_password_focus();
        }
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
            lv_label_set_text(wifi_status_label, "Scan already in progress");
        } else {
            lv_label_set_text(wifi_status_label, "Failed to start scan");
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
        lv_label_set_text(wifi_status_label, "Scan first, then select a network");
        return;
    }

    strncpy(password, lv_textarea_get_text(wifi_password_textarea),
            sizeof(password) - 1);

    lv_label_set_text(wifi_status_label, "Starting connection...");
    
    set_wifi_inputs_busy(true);

    esp_err_t ret = wifi_svc_connect_async(ssid, password);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            lv_label_set_text(wifi_status_label, "Already connecting...");
        } else {
            lv_label_set_text(wifi_status_label, "Failed to start connect");
        }
        set_wifi_inputs_busy(false);
    }
}

static void wifi_forget_event_cb(lv_event_t *event)
{
    (void)event;

    char ssid[WIFI_SSID_BUF_SIZE] = {0};
    lv_dropdown_get_selected_str(wifi_ssid_dropdown, ssid, sizeof(ssid));

    if (ssid[0] == '\0' || strcmp(ssid, SSID_PLACEHOLDER) == 0) {
        lv_label_set_text(wifi_status_label, "Select a network first");
        return;
    }

    char dummy[WIFI_PASS_BUF_SIZE];
    if (!wifi_svc_get_saved_password(ssid, dummy, sizeof(dummy))) {
        lv_label_set_text_fmt(wifi_status_label, "No saved password for: %s", ssid);
        return;
    }

    esp_err_t ret = wifi_svc_forget(ssid);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(wifi_status_label, "Forget failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "wifi_svc_forget: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Forgot password for '%s'", ssid);

    /* Unlock the fields so the user can type a new password and
       connect. Also covers the auto-connect case where no scan has
       run yet — Forget is treated as an explicit "edit this network"
       action. */
    s_wifi_scanned = true;
    lv_obj_clear_state(wifi_ssid_dropdown,     LV_STATE_DISABLED);

    lv_textarea_set_text(wifi_password_textarea, "");

    /* Reopen the on-screen keyboard and focus the textarea. */
    lv_obj_clear_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    wifi_password_focus();

    lv_obj_add_flag(wifi_forget_button, LV_OBJ_FLAG_HIDDEN);
    update_connect_button_state();

    lv_label_set_text_fmt(wifi_status_label, "Forgot password for: %s", ssid);
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

    lv_dropdown_set_options(wifi_ssid_dropdown, count ? options : SSID_NO_RESULTS);
    lv_label_set_text_fmt(wifi_status_label, "%u networks found", (unsigned)count);

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
        lv_label_set_text(wifi_status_label, "Scanning...");
        set_wifi_inputs_busy(true);
        lv_obj_clear_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_forget_button, LV_OBJ_FLAG_HIDDEN);
        break;

    case MSG_WIFI_CONNECTING:
        set_wifi_icon(false, true);
        if (wifi_svc_get_target_ssid(ssid, sizeof(ssid)) == ESP_OK) {
            lv_label_set_text_fmt(wifi_status_label, "Connecting to: %s...", ssid);
        } else {
            lv_label_set_text(wifi_status_label, "Connecting...");
        }
        set_wifi_inputs_busy(true);
        break;

    case MSG_WIFI_CONNECTED: {
        set_wifi_icon(true, false);

        if (wifi_svc_get_current_ssid(ssid, sizeof(ssid)) == ESP_OK) {
            char selected[WIFI_SSID_BUF_SIZE] = {0};
            lv_dropdown_get_selected_str(wifi_ssid_dropdown, selected, sizeof(selected));
            if (strcmp(selected, SSID_PLACEHOLDER) == 0) {
                lv_dropdown_set_options(wifi_ssid_dropdown, ssid);
                lv_dropdown_set_selected(wifi_ssid_dropdown, 0);
                lv_obj_send_event(wifi_ssid_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
            } else {
                lv_obj_clear_flag(wifi_forget_button, LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text_fmt(wifi_status_label, "Connected to: %s", ssid);
        } else {
            lv_label_set_text(wifi_status_label, "Connected");
        }

        set_wifi_inputs_busy(false);
        lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    case MSG_WIFI_RECONNECTING:
        set_wifi_icon(false, true);
        lv_label_set_text(wifi_status_label, "Reconnecting...");
        lv_obj_add_state(wifi_ssid_dropdown, LV_STATE_DISABLED);
        wifi_password_enable(false);
        break;

    case MSG_WIFI_DISCONNECTING:
        set_wifi_icon(false, true);
        lv_label_set_text(wifi_status_label, "Switching network...");
        lv_obj_add_state(wifi_ssid_dropdown, LV_STATE_DISABLED);
        wifi_password_enable(false);
        break;

    case MSG_WIFI_DISCONNECTED: {
        int32_t reason = wifi_svc_get_last_disconnect_reason();
        lv_label_set_text_fmt(wifi_status_label, "%s", disconnect_reason_str(reason));
        set_wifi_icon(false, false);
        wifi_password_defocus();
        set_wifi_inputs_busy(false);
        break;
    }

    case MSG_WIFI_SCAN_SUCCESS: {
        static char ssids[WIFI_SCAN_MAX_RESULTS][WIFI_SSID_BUF_SIZE];
        size_t n = wifi_svc_scan_get_results(ssids, WIFI_SCAN_MAX_RESULTS);
        s_wifi_scanned = true;
        set_wifi_inputs_busy(false);   /* unlock before firing events */
        set_scan_results(ssids, n);
        set_wifi_icon(wifi_svc_is_connected(), false);
        lv_obj_add_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    case MSG_WIFI_SCAN_FAILED:
        lv_label_set_text(wifi_status_label, "WiFi scan failed");
        set_wifi_icon(wifi_svc_is_connected(), false);
        set_wifi_inputs_busy(false);
        lv_obj_add_flag(wifi_spinner, LV_OBJ_FLAG_HIDDEN);
        break;

    default:
        ESP_LOGW(TAG, "Unhandled wifi event id=%ld", (long)id);
        break;
    }
}

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
    lv_obj_set_size(s_tv, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_tv);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);

    lv_obj_set_style_pad_all(s_tv, 0, 0);
    lv_obj_set_style_border_width(s_tv, 0, 0);

    s_page_main = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_NONE);
    s_page_wifi = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_NONE);

    lv_obj_clear_flag(s_page_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_page_wifi, LV_OBJ_FLAG_SCROLLABLE);

    create_main_page(s_page_main);
    create_wifi_page(s_page_wifi);

    /* WiFi icon on top layer */
    wifi_icon = lv_label_create(lv_layer_top());
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_22, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_MID, 0, 8);
    set_wifi_icon(false, false);

    /* --- Theme toggle --- */
    lv_obj_t *theme_switch = lv_switch_create(lv_layer_top());
    lv_obj_align(theme_switch, LV_ALIGN_TOP_LEFT, 8, 8);
    if (s_dark_theme) lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(theme_switch, theme_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    apply_theme();
    lv_timer_create(wifi_apply_cb, WIFI_EVENT_POLL_MS, NULL);
    ui_update_clock();
    lvgl_port_unlock();
}

static void create_main_page(lv_obj_t *parent)
{
    /* --- Clock --- */
    clock_label = lv_label_create(parent);
    lv_obj_set_style_text_font(clock_label, &DroidSansMono_128, 0);
    lv_obj_align(clock_label, LV_ALIGN_TOP_MID, CLOCK_X, CLOCK_Y);
    lv_label_set_text(clock_label, "--:--:--");

    /* --- CO2 label + value --- */
    label_txt_co2 = lv_label_create(parent);
    lv_obj_set_style_text_font(label_txt_co2, &RobotoMono_80, 0);
    lv_obj_align(label_txt_co2, LV_ALIGN_CENTER, COL_CO2_X, CO2_TITLE_Y);
    lv_label_set_text(label_txt_co2, "CO2");

    label_txt_ppm = lv_label_create(parent);
    lv_obj_set_style_text_font(label_txt_ppm, &RobotoMono_80, 0);
    lv_obj_align(label_txt_ppm, LV_ALIGN_CENTER, COL_CO2_X, CO2_UNITS_Y);
    lv_label_set_text(label_txt_ppm, "ppm");

    label_co2 = lv_label_create(parent);
    lv_obj_set_style_text_font(label_co2, &RobotoMono_88, 0);
    lv_obj_align(label_co2, LV_ALIGN_CENTER, COL_CO2_X, CO2_VALUE_Y);
    lv_label_set_text(label_co2, "0000");

    /* --- CO2 arcs --- */
    lv_style_init(&style_arc_co2_bg);
    lv_style_set_arc_width(&style_arc_co2_bg, ARC_CO2_WIDTH);
    lv_style_set_arc_rounded(&style_arc_co2_bg, false);
    lv_style_set_arc_color(&style_arc_co2_bg, lv_color_hex(0x404040));

    for (int i = 0; i < ARC_SEGMENT_COUNT; i++) {
        arc_co2[i] = lv_arc_create(parent);
        lv_obj_set_size(arc_co2[i], ARC_CO2_SIZE, ARC_CO2_SIZE);
        lv_obj_align(arc_co2[i], LV_ALIGN_CENTER, COL_CO2_X, CO2_VALUE_Y);

        lv_obj_remove_style(arc_co2[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc_co2[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_arc_color(arc_co2[i], lv_color_hex(arc_co2_palette_hex[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc_co2[i], ARC_CO2_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc_co2[i], false, LV_PART_INDICATOR);
        lv_obj_add_style(arc_co2[i], &style_arc_co2_bg, LV_PART_MAIN);

        lv_arc_set_range(arc_co2[i], CO2_ARC_MIN + i * CO2_ARC_STEP, CO2_ARC_MIN + (i + 1) * CO2_ARC_STEP);

        int arc_start_angle = ARC_START_ANGLE + i * (ARC_WIDTH_ANGLE + ARC_GAP_ANGLE);
        int arc_end_angle   = arc_start_angle + ARC_WIDTH_ANGLE;
        lv_arc_set_bg_start_angle(arc_co2[i], angle_mod360(arc_start_angle));
        lv_arc_set_bg_end_angle(arc_co2[i],   angle_mod360(arc_end_angle));
        lv_arc_set_mode(arc_co2[i], LV_ARC_MODE_NORMAL);
    }

    /* --- Temperature --- */
    label_temp = lv_label_create(parent);
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_48, 0);
    lv_obj_align(label_temp, LV_ALIGN_CENTER, COL_TEMP_X, COL_TEMP_HUM_Y);
    lv_label_set_text(label_temp, "00.0");

    lv_style_init(&style_arc_temp_bg);
    lv_style_set_arc_width(&style_arc_temp_bg, ARC_TEMP_WIDTH);
    lv_style_set_arc_rounded(&style_arc_temp_bg, false);
    lv_style_set_arc_color(&style_arc_temp_bg, lv_color_hex(0x404040));

    for (int i = 0; i < ARC_SEGMENT_COUNT; i++) {
        arc_temp[i] = lv_arc_create(parent);
        lv_obj_set_size(arc_temp[i], ARC_TEMP_SIZE, ARC_TEMP_SIZE);
        lv_obj_align(arc_temp[i], LV_ALIGN_CENTER, COL_TEMP_X, COL_TEMP_HUM_Y);

        lv_obj_remove_style(arc_temp[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc_temp[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_arc_color(arc_temp[i], lv_color_hex(arc_temp_hum_palette_hex[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc_temp[i], ARC_TEMP_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc_temp[i], false, LV_PART_INDICATOR);
        lv_obj_add_style(arc_temp[i], &style_arc_temp_bg, LV_PART_MAIN);

        lv_arc_set_range(arc_temp[i], TEMP_ARC_MIN + i * TEMP_ARC_STEP, TEMP_ARC_MIN + (i + 1) * TEMP_ARC_STEP);

        int arc_start_angle = ARC_START_ANGLE + i * (ARC_WIDTH_ANGLE + ARC_GAP_ANGLE);
        int arc_end_angle   = arc_start_angle + ARC_WIDTH_ANGLE;
        lv_arc_set_bg_start_angle(arc_temp[i], angle_mod360(arc_start_angle));
        lv_arc_set_bg_end_angle(arc_temp[i],   angle_mod360(arc_end_angle));
        lv_arc_set_mode(arc_temp[i], LV_ARC_MODE_NORMAL);
    }

    /* --- Humidity --- */
    label_hum = lv_label_create(parent);
    lv_obj_set_style_text_font(label_hum, &lv_font_montserrat_48, 0);
    lv_obj_align(label_hum, LV_ALIGN_CENTER, COL_HUM_X, COL_TEMP_HUM_Y);
    lv_label_set_text(label_hum, "00");

    lv_style_init(&style_arc_hum_bg);
    lv_style_set_arc_width(&style_arc_hum_bg, ARC_TEMP_WIDTH);
    lv_style_set_arc_rounded(&style_arc_hum_bg, false);
    lv_style_set_arc_color(&style_arc_hum_bg, lv_color_hex(0x404040));

    for (int i = 0; i < ARC_SEGMENT_COUNT; i++) {
        arc_hum[i] = lv_arc_create(parent);
        lv_obj_set_size(arc_hum[i], ARC_TEMP_SIZE, ARC_TEMP_SIZE);
        lv_obj_align(arc_hum[i], LV_ALIGN_CENTER, COL_HUM_X, COL_TEMP_HUM_Y);

        lv_obj_remove_style(arc_hum[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc_hum[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_arc_color(arc_hum[i], lv_color_hex(arc_temp_hum_palette_hex[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc_hum[i], ARC_TEMP_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc_hum[i], false, LV_PART_INDICATOR);
        lv_obj_add_style(arc_hum[i], &style_arc_hum_bg, LV_PART_MAIN);

        lv_arc_set_range(arc_hum[i], HUM_ARC_MIN + i * HUM_ARC_STEP, HUM_ARC_MIN + (i + 1) * HUM_ARC_STEP);

        int arc_start_angle = ARC_START_ANGLE + i * (ARC_WIDTH_ANGLE + ARC_GAP_ANGLE);
        int arc_end_angle   = arc_start_angle + ARC_WIDTH_ANGLE;
        lv_arc_set_bg_start_angle(arc_hum[i], angle_mod360(arc_start_angle));
        lv_arc_set_bg_end_angle(arc_hum[i],   angle_mod360(arc_end_angle));
        lv_arc_set_mode(arc_hum[i], LV_ARC_MODE_NORMAL);
    }

    /* --- Settings button --- */
    lv_obj_t *settings_btn = lv_button_create(parent);
    lv_obj_set_size(settings_btn, 36, 36);
    lv_obj_align(settings_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_t *settings_lbl = lv_label_create(settings_btn);
    lv_label_set_text(settings_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_center(settings_lbl);
    lv_obj_set_style_text_font(settings_btn, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(settings_btn, open_settings_cb, LV_EVENT_CLICKED, NULL);

    anim_startup();
}

static void create_wifi_page(lv_obj_t *parent)
{
    /* --- Title --- */
    lv_obj_t *wifi_title = lv_label_create(parent);
    lv_label_set_text(wifi_title, "WiFi settings");
    lv_obj_align(wifi_title, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_text_font(wifi_title, &lv_font_montserrat_22, LV_PART_MAIN);

    /* --- SSID dropdown --- */
    wifi_ssid_dropdown = lv_dropdown_create(parent);
    lv_obj_set_width(wifi_ssid_dropdown, 312);
    lv_obj_align(wifi_ssid_dropdown, LV_ALIGN_TOP_MID, WIFI_LEFT_X, 48);
    lv_dropdown_set_options(wifi_ssid_dropdown, SSID_PLACEHOLDER);
    lv_obj_set_style_text_font(wifi_ssid_dropdown, &lv_font_montserrat_22, LV_PART_MAIN);

    lv_obj_t *list = lv_dropdown_get_list(wifi_ssid_dropdown);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_add_state(wifi_ssid_dropdown, LV_STATE_DISABLED);

    /* --- Scan button --- */
    wifi_scan_button = lv_button_create(parent);
    lv_obj_set_size(wifi_scan_button, 128, 48);
    lv_obj_align(wifi_scan_button, LV_ALIGN_TOP_MID, WIFI_LEFT_X, 128);
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
    lv_obj_align(wifi_password_textarea, LV_ALIGN_TOP_MID, WIFI_RIGHT_X, 48);
    lv_textarea_set_placeholder_text(wifi_password_textarea, "WiFi password");
    lv_textarea_set_password_mode(wifi_password_textarea, true);
    lv_obj_set_style_text_font(wifi_password_textarea, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(wifi_password_textarea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(wifi_password_textarea, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(wifi_password_textarea, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(wifi_password_textarea, 45, LV_PART_MAIN);
    wifi_password_enable(false);

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
    wifi_password_defocus();
    lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);

    /* --- Connect button --- */
    wifi_connect_button = lv_button_create(parent);
    lv_obj_set_size(wifi_connect_button, 128, 48);
    lv_obj_align(wifi_connect_button, LV_ALIGN_TOP_MID, WIFI_RIGHT_X, 128);
    lv_obj_t *connect_label = lv_label_create(wifi_connect_button);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_add_state(wifi_connect_button, LV_STATE_DISABLED);

    /* --- Forget button (visible only when a saved password exists) --- */
    wifi_forget_button = lv_button_create(parent);
    lv_obj_set_size(wifi_forget_button, 48, 48);
    lv_obj_align(wifi_forget_button, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_opa(wifi_forget_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(wifi_forget_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_forget_button, 0, 0);

    lv_obj_t *forget_label = lv_label_create(wifi_forget_button);
    lv_label_set_text(forget_label, LV_SYMBOL_TRASH);
    lv_obj_center(forget_label);
    lv_obj_set_style_text_font(wifi_forget_button, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_add_flag(wifi_forget_button, LV_OBJ_FLAG_HIDDEN);

    /* --- Status label --- */
    wifi_status_label = lv_label_create(parent);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_MID, 0, 184);
    lv_obj_set_width(wifi_status_label, 400);
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wifi_status_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(wifi_status_label, "Select a network");

    /* --- Back button --- */
    lv_obj_t *back_btn = lv_button_create(parent);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(back_btn, close_settings_cb, LV_EVENT_CLICKED, NULL);

    /* --- Event callbacks --- */
    lv_obj_add_event_cb(wifi_ssid_dropdown, wifi_dropdown_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(wifi_scan_button, wifi_scan_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(wifi_connect_button, wifi_connect_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(wifi_forget_button, wifi_forget_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(password_btn, toggle_password_visibility_cb, LV_EVENT_CLICKED, wifi_password_textarea);
}