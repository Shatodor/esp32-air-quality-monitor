#include "unity.h"
#include "esp_err.h"
#include "rtc_svc.h"
#include "driver/i2c_master.h"
#include "clock_mock.h"

/* DS3231 register addresses (mirror of what rtc_svc uses). */
#define REG_SEC    0x00
#define REG_MIN    0x01
#define REG_HOUR   0x02
#define REG_WDAY   0x03
#define REG_MDAY   0x04
#define REG_MON    0x05
#define REG_YEAR   0x06
#define REG_STATUS 0x0F
#define OSF_BIT    0x80

/* 2024-01-15 12:30:45 UTC */
#define EPOCH_VALID 1705321845

/* ---- init: I2C errors ---- */

static void test_init_add_device_error_propagates(void)
{
    mock_i2c_reset();
    mock_i2c_set_add_result(ESP_ERR_NO_MEM);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
        rtc_svc_init((i2c_master_bus_handle_t)1));
}

static void test_init_osf_read_error_propagates(void)
{
    mock_i2c_reset();
    mock_i2c_set_transmit_receive_result(ESP_FAIL);

    TEST_ASSERT_EQUAL(ESP_FAIL,
        rtc_svc_init((i2c_master_bus_handle_t)1));
}

/* ---- init: OSF handling ---- */

static void test_init_osf_set_returns_invalid_state(void)
{
    mock_i2c_reset();
    mock_i2c_set_reg(REG_STATUS, OSF_BIT);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        rtc_svc_init((i2c_master_bus_handle_t)1));

    /* rtc_svc_init should clear OSF after detecting it. */
    TEST_ASSERT_EQUAL_HEX8(0, mock_i2c_get_reg(REG_STATUS) & OSF_BIT);
}

/* ---- init: year plausibility ---- */

static void test_init_year_before_2024_rejected(void)
{
    mock_i2c_reset();
    /* 2023-06-01 00:00:00 */
    mock_i2c_set_reg(REG_SEC,    0x00);
    mock_i2c_set_reg(REG_MIN,    0x00);
    mock_i2c_set_reg(REG_HOUR,   0x00);
    mock_i2c_set_reg(REG_WDAY,   0x01);
    mock_i2c_set_reg(REG_MDAY,   0x01);
    mock_i2c_set_reg(REG_MON,    0x06);
    mock_i2c_set_reg(REG_YEAR,   0x23);   /* 23 -> 2023 */
    mock_i2c_set_reg(REG_STATUS, 0);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        rtc_svc_init((i2c_master_bus_handle_t)1));
}

/* ---- init: valid time sets system time ---- */

static void test_init_valid_time_sets_system_clock(void)
{
    mock_i2c_reset();

    /* 2024-01-15 12:30:45 UTC in BCD */
    mock_i2c_set_reg(REG_SEC,    0x45);
    mock_i2c_set_reg(REG_MIN,    0x30);
    mock_i2c_set_reg(REG_HOUR,   0x12);
    mock_i2c_set_reg(REG_WDAY,   0x02);   /* Monday, DS numbering */
    mock_i2c_set_reg(REG_MDAY,   0x15);
    mock_i2c_set_reg(REG_MON,    0x01);
    mock_i2c_set_reg(REG_YEAR,   0x24);   /* 2024 */
    mock_i2c_set_reg(REG_STATUS, 0);

    TEST_ASSERT_EQUAL(ESP_OK,
        rtc_svc_init((i2c_master_bus_handle_t)1));

    /* rtc_svc passes the epoch to settimeofday; verify via the mock
       instead of calling time() — the latter is not wrappable on
       MinGW (inline into _time64). */
    TEST_ASSERT_EQUAL_INT(EPOCH_VALID,
        (int)mock_clock_get_last_settimeofday());
}

/* ---- set_from_system: relies on --wrap=time ---- */
/* MinGW expands time() into _time64 in libc, so --wrap=time does not
   intercept it. These tests only run on Linux, where --wrap works. */

#ifndef _WIN32

static void test_set_invalid_system_time_no_write(void)
{
    mock_i2c_reset();
    rtc_svc_init((i2c_master_bus_handle_t)1);
    mock_i2c_reset();    /* discard writes from init */

    mock_clock_set(0);   /* epoch 1970 — below 2024 */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rtc_svc_set_from_system());
    TEST_ASSERT_EQUAL_INT(0, mock_i2c_get_write_count());
}

static void test_set_writes_correct_bcd_bytes(void)
{
    mock_i2c_reset();
    rtc_svc_init((i2c_master_bus_handle_t)1);
    mock_i2c_reset();

    mock_clock_set(EPOCH_VALID);   /* 2024-01-15 12:30:45 UTC */
    TEST_ASSERT_EQUAL(ESP_OK, rtc_svc_set_from_system());

    TEST_ASSERT_EQUAL_HEX8(0x45, mock_i2c_get_reg(REG_SEC));
    TEST_ASSERT_EQUAL_HEX8(0x30, mock_i2c_get_reg(REG_MIN));
    TEST_ASSERT_EQUAL_HEX8(0x12, mock_i2c_get_reg(REG_HOUR));
    TEST_ASSERT_EQUAL_HEX8(0x15, mock_i2c_get_reg(REG_MDAY));
    TEST_ASSERT_EQUAL_HEX8(0x01, mock_i2c_get_reg(REG_MON));
    TEST_ASSERT_EQUAL_HEX8(0x24, mock_i2c_get_reg(REG_YEAR));
}

static void test_set_clears_osf_after_write(void)
{
    mock_i2c_reset();
    rtc_svc_init((i2c_master_bus_handle_t)1);
    mock_i2c_reset();

    mock_i2c_set_reg(REG_STATUS, OSF_BIT);
    mock_clock_set(EPOCH_VALID);

    TEST_ASSERT_EQUAL(ESP_OK, rtc_svc_set_from_system());
    TEST_ASSERT_EQUAL_HEX8(0, mock_i2c_get_reg(REG_STATUS) & OSF_BIT);
}

static void test_set_transmit_error_propagates(void)
{
    mock_i2c_reset();
    rtc_svc_init((i2c_master_bus_handle_t)1);
    mock_i2c_reset();

    mock_clock_set(EPOCH_VALID);
    mock_i2c_set_transmit_result(ESP_FAIL);

    TEST_ASSERT_EQUAL(ESP_FAIL, rtc_svc_set_from_system());
}

#endif /* !_WIN32 */

/* ---- runner ---- */

void run_rtc_svc_tests(void)
{
    RUN_TEST(test_init_add_device_error_propagates);
    RUN_TEST(test_init_osf_read_error_propagates);
    RUN_TEST(test_init_osf_set_returns_invalid_state);
    RUN_TEST(test_init_year_before_2024_rejected);
    RUN_TEST(test_init_valid_time_sets_system_clock);

#ifndef _WIN32
    RUN_TEST(test_set_invalid_system_time_no_write);
    RUN_TEST(test_set_writes_correct_bcd_bytes);
    RUN_TEST(test_set_clears_osf_after_write);
    RUN_TEST(test_set_transmit_error_propagates);
#endif
}