#include "unity.h"
#include "esp_err.h"
#include "wifi_svc.h"

void mock_event_reset(void);
void mock_wifi_reset(void);
void mock_nvs_reset(void);

static void test_connect_null_ssid_rejected(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_svc_connect(NULL, "pw"));
}

static void test_connect_empty_ssid_rejected(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_svc_connect("", "pw"));
}

static void test_connect_too_long_ssid_rejected(void)
{
    char long_ssid[40];
    for (int i = 0; i < 33; i++) long_ssid[i] = 'a';
    long_ssid[33] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_svc_connect(long_ssid, "pw"));
}

static void test_connect_too_long_password_rejected(void)
{
    char long_pw[70];
    for (int i = 0; i < 65; i++) long_pw[i] = 'p';
    long_pw[65] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_svc_connect("Freedom", long_pw));
}

static void test_connect_null_password_accepted_as_empty(void)
{
    mock_event_reset();
    mock_wifi_reset();
    mock_nvs_reset();
    esp_err_t ret = wifi_svc_connect("Freedom", NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

void run_wifi_svc_tests(void)
{
    RUN_TEST(test_connect_null_ssid_rejected);
    RUN_TEST(test_connect_empty_ssid_rejected);
    RUN_TEST(test_connect_too_long_ssid_rejected);
    RUN_TEST(test_connect_too_long_password_rejected);
    RUN_TEST(test_connect_null_password_accepted_as_empty);
}
