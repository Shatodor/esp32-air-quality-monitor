#ifndef RTC_SVC_H
#define RTC_SVC_H

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/*
 * Minimal DS3231 RTC service built on the shared i2c_master bus.
 *
 * Time semantics:
 *   - RTC stores UTC. TZ (set in app_main) only affects display.
 *   - On init: if RTC time is valid and the oscillator-stop flag is clear,
 *     system time is set from RTC.
 *   - After a successful SNTP sync, call rtc_svc_set_from_system() to
 *     persist accurate time in the battery-backed RTC.
 *
 * Threading: init must run before any caller. R/W helpers are safe to call
 * from one task at a time (no internal mutex).
 */

esp_err_t rtc_svc_init(i2c_master_bus_handle_t bus);
esp_err_t rtc_svc_set_from_system(void);

#endif // RTC_SVC_H