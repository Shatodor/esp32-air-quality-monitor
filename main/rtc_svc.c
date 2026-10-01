#include "rtc_svc.h"

#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define TAG "RTC_SVC"

/* ---- DS3231 registers & constants ---- */

#define DS3231_ADDR             0x68
#define DS3231_REG_TIME         0x00
#define DS3231_REG_STATUS       0x0F
#define DS3231_TIME_LEN         7

#define DS3231_STATUS_OSF       0x80   /* bit 7: oscillator stop flag */

#define I2C_RTC_FREQ_HZ         100000
#define I2C_RTC_TIMEOUT_MS      100

/* Anything below this year is treated as garbage */
#define RTC_MIN_VALID_YEAR      2024

static i2c_master_dev_handle_t s_rtc;

/* ---- BCD ---- */

static inline uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }
static inline uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

/* ---- Raw helpers ---- */

static esp_err_t ds3231_read_time(struct tm *out)
{
    const uint8_t reg = DS3231_REG_TIME;
    uint8_t buf[DS3231_TIME_LEN] = {0};

    esp_err_t ret = i2c_master_transmit_receive(s_rtc, &reg, 1,
                                                buf, sizeof(buf),
                                                I2C_RTC_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;

    out->tm_sec   = bcd2bin(buf[0] & 0x7F);
    out->tm_min   = bcd2bin(buf[1] & 0x7F);
    out->tm_hour  = bcd2bin(buf[2] & 0x3F);   /* force 24h interpretation */
    out->tm_wday  = bcd2bin(buf[3] & 0x07) - 1; /* DS3231: 1..7 -> tm: 0..6 */
    out->tm_mday  = bcd2bin(buf[4] & 0x3F);
    out->tm_mon   = bcd2bin(buf[5] & 0x1F) - 1; /* tm: 0..11 */
    out->tm_year  = bcd2bin(buf[6]) + 100;      /* 2000..2099 -> 100..199 */
    out->tm_isdst = 0;

    return ESP_OK;
}

static esp_err_t ds3231_write_time(const struct tm *in)
{
    uint8_t buf[DS3231_TIME_LEN + 1];
    buf[0] = DS3231_REG_TIME;
    buf[1] = bin2bcd(in->tm_sec);
    buf[2] = bin2bcd(in->tm_min);
    buf[3] = bin2bcd(in->tm_hour);          /* 24h mode, bit 6 = 0 */
    buf[4] = bin2bcd(in->tm_wday + 1);      /* tm 0..6 -> DS 1..7 */
    buf[5] = bin2bcd(in->tm_mday);
    buf[6] = bin2bcd(in->tm_mon + 1);       /* + century flag if year >= 2100 */
    buf[7] = bin2bcd(in->tm_year >= 100 ? in->tm_year - 100 : in->tm_year);

    return i2c_master_transmit(s_rtc, buf, sizeof(buf), I2C_RTC_TIMEOUT_MS);
}

static esp_err_t ds3231_check_osf(bool *stopped)
{
    const uint8_t reg = DS3231_REG_STATUS;
    uint8_t val = 0;

    esp_err_t ret = i2c_master_transmit_receive(s_rtc, &reg, 1, &val, 1,
                                                I2C_RTC_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;

    *stopped = (val & DS3231_STATUS_OSF) != 0;
    return ESP_OK;
}

static esp_err_t ds3231_clear_osf(void)
{
    const uint8_t reg = DS3231_REG_STATUS;
    uint8_t val = 0;

    esp_err_t ret = i2c_master_transmit_receive(s_rtc, &reg, 1, &val, 1,
                                                I2C_RTC_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;

    val &= ~DS3231_STATUS_OSF;

    uint8_t wr[2] = { DS3231_REG_STATUS, val };
    return i2c_master_transmit(s_rtc, wr, 2, I2C_RTC_TIMEOUT_MS);
}


/* Convert struct tm (interpreted as UTC) to time_t.
   timegm() is a POSIX extension; in ESP-IDF newlib it is not always visible.
   This is a portable equivalent. */
static time_t utc_to_time_t(struct tm *tm)
{
    char tz_backup[32] = {0};
    const char *tz = getenv("TZ");
    bool had_tz = (tz != NULL);
    if (had_tz) {
        strncpy(tz_backup, tz, sizeof(tz_backup) - 1);
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(tm);

    if (had_tz) {
        setenv("TZ", tz_backup, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    return t;
}

/* ---- Public API ---- */

esp_err_t rtc_svc_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS3231_ADDR,
        .scl_speed_hz    = I2C_RTC_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &s_rtc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add DS3231: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Did we lose power since last write? */
    bool osf = false;
    ret = ds3231_check_osf(&osf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read OSF: %s", esp_err_to_name(ret));
        return ret;
    }

    if (osf) {
        ESP_LOGW(TAG, "oscillator stopped (battery lost?), RTC time is unreliable");
        /* Clear flag so we can detect the next event. Time is not used. */
        ds3231_clear_osf();
        return ESP_ERR_INVALID_STATE;
    }

    struct tm rtc_time = {0};
    ret = ds3231_read_time(&rtc_time);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read time: %s", esp_err_to_name(ret));
        return ret;
    }

    int year = rtc_time.tm_year + 1900;
    if (year < RTC_MIN_VALID_YEAR) {
        ESP_LOGW(TAG, "RTC year %d is implausible, ignoring", year);
        return ESP_ERR_INVALID_STATE;
    }

    struct timeval tv = {
        .tv_sec  = utc_to_time_t(&rtc_time),
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "System time set from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
             year, rtc_time.tm_mon + 1, rtc_time.tm_mday,
             rtc_time.tm_hour, rtc_time.tm_min, rtc_time.tm_sec);
    return ESP_OK;
}

esp_err_t rtc_svc_set_from_system(void)
{
    time_t now = 0;
    time(&now);

    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);

    if (tm_utc.tm_year + 1900 < RTC_MIN_VALID_YEAR) {
        ESP_LOGW(TAG, "system time invalid, not writing to RTC");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ds3231_write_time(&tm_utc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write time: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Time is now trustworthy — OSF is safe to clear. */
    ds3231_clear_osf();

    ESP_LOGI(TAG, "RTC updated: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return ESP_OK;
}