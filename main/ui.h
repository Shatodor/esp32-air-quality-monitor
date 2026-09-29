#ifndef UI_H
#define UI_H

/*
 * UI module: widgets + WiFi event handling.
 *
 * Threading:
 *   ui_create             — any task (takes LVGL lock)
 *   ui_update_*           — LVGL timer only
 *   ui_wifi_event_handler — event loop task (no LVGL)
 */

#include "lvgl.h"
#include "esp_event.h"
#include <stdbool.h>

/* Create all widgets. Takes LVGL lock internally. */
void ui_create(void);

/* WIFI_SVC_EVENTS handler. Register via esp_event_handler_instance_register. */
void ui_wifi_event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data);

/* Update clock label. Call from LVGL timer. */
void ui_update_clock(void);

/* Update sensor readings. Call from LVGL timer. */
void ui_update_sensors(float co2, float temperature, float humidity);

#endif // UI_H