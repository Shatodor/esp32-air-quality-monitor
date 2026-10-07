#include "unity.h"
#include "esp_err.h"
#include "wifi_creds.h"
#include "nvs_flash.h"

void mock_nvs_reset(void);
void mock_nvs_set_open_result(esp_err_t r);
void mock_nvs_set_set_str_result(esp_err_t r);
void mock_nvs_set_commit_result(esp_err_t r);
int  mock_nvs_get_write_count(void);

/* ---- get: input validation ---- */

static void test_get_null_ssid_returns_false(void)
{
    char buf[64];
    TEST_ASSERT_FALSE(wifi_creds_get(NULL, buf, sizeof(buf)));
}

static void test_get_null_out_returns_false(void)
{
    TEST_ASSERT_FALSE(wifi_creds_get("TestSSID", NULL, 64));
}

static void test_get_zero_len_returns_false(void)
{
    char buf[64];
    TEST_ASSERT_FALSE(wifi_creds_get("TestSSID", buf, 0));
}

/* ---- get: storage miss ---- */

static void test_get_missing_key_returns_false(void)
{
    mock_nvs_reset();
    char buf[64];
    TEST_ASSERT_FALSE(wifi_creds_get("TestSSID", buf, sizeof(buf)));
}

/* ---- save + get round-trip ---- */

static void test_save_then_get_returns_same_password(void)
{
    mock_nvs_reset();
    wifi_creds_save("TestSSID", "passwr2");

    char buf[64] = {0};
    TEST_ASSERT_TRUE(wifi_creds_get("TestSSID", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("passwr2", buf);
}

static void test_two_ssids_are_isolated(void)
{
    mock_nvs_reset();
    wifi_creds_save("TestSSID", "alpha");
    wifi_creds_save("HomeNet", "beta");

    char buf[64] = {0};
    TEST_ASSERT_TRUE(wifi_creds_get("TestSSID", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("alpha", buf);
    TEST_ASSERT_TRUE(wifi_creds_get("HomeNet", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("beta", buf);
}

/* ---- save: idempotency ---- */

static void test_save_same_password_is_noop(void)
{
    mock_nvs_reset();
    wifi_creds_save("TestSSID", "passwr2");
    int writes_after_first = mock_nvs_get_write_count();

    wifi_creds_save("TestSSID", "passwr2");
    TEST_ASSERT_EQUAL_INT(writes_after_first, mock_nvs_get_write_count());
}

static void test_save_different_password_updates(void)
{
    mock_nvs_reset();
    wifi_creds_save("TestSSID", "old");
    int writes_after_first = mock_nvs_get_write_count();

    wifi_creds_save("TestSSID", "new");
    TEST_ASSERT_GREATER_THAN(writes_after_first, mock_nvs_get_write_count());

    char buf[64] = {0};
    TEST_ASSERT_TRUE(wifi_creds_get("TestSSID", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("new", buf);
}

/* ---- save: input validation ---- */

static void test_save_null_ssid_is_noop(void)
{
    mock_nvs_reset();
    wifi_creds_save(NULL, "pw");
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_get_write_count());
}

static void test_save_empty_ssid_is_noop(void)
{
    mock_nvs_reset();
    wifi_creds_save("", "pw");
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_get_write_count());
}

static void test_save_null_password_is_noop(void)
{
    mock_nvs_reset();
    wifi_creds_save("TestSSID", NULL);
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_get_write_count());
}

/* ---- forget ---- */

static void test_forget_existing_key_returns_ok(void)
{
    mock_nvs_reset();
    wifi_creds_save("TestSSID", "passwr2");

    TEST_ASSERT_EQUAL(ESP_OK, wifi_creds_forget("TestSSID"));

    char buf[64];
    TEST_ASSERT_FALSE(wifi_creds_get("TestSSID", buf, sizeof(buf)));
}

static void test_forget_missing_key_returns_ok(void)
{
    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, wifi_creds_forget("TestSSID"));
}

static void test_forget_null_ssid_rejected(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_creds_forget(NULL));
}

static void test_forget_empty_ssid_rejected(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_creds_forget(""));
}

static void test_forget_does_not_affect_other_ssid(void)
{
    mock_nvs_reset();
    wifi_creds_save("TestSSID", "alpha");
    wifi_creds_save("HomeNet", "beta");

    TEST_ASSERT_EQUAL(ESP_OK, wifi_creds_forget("TestSSID"));

    char buf[64] = {0};
    TEST_ASSERT_TRUE(wifi_creds_get("HomeNet", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("beta", buf);
}

/* ---- long values ---- */

static void test_long_password_roundtrip(void)
{
    mock_nvs_reset();
    char long_pw[64];
    for (int i = 0; i < 63; i++) long_pw[i] = 'a' + (i % 26);
    long_pw[63] = '\0';

    wifi_creds_save("TestSSID", long_pw);

    char buf[64] = {0};
    TEST_ASSERT_TRUE(wifi_creds_get("TestSSID", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(long_pw, buf);
}

/* ---- runner ---- */

void run_wifi_creds_tests(void)
{
    RUN_TEST(test_get_null_ssid_returns_false);
    RUN_TEST(test_get_null_out_returns_false);
    RUN_TEST(test_get_zero_len_returns_false);
    RUN_TEST(test_get_missing_key_returns_false);

    RUN_TEST(test_save_then_get_returns_same_password);
    RUN_TEST(test_two_ssids_are_isolated);

    RUN_TEST(test_save_same_password_is_noop);
    RUN_TEST(test_save_different_password_updates);

    RUN_TEST(test_save_null_ssid_is_noop);
    RUN_TEST(test_save_empty_ssid_is_noop);
    RUN_TEST(test_save_null_password_is_noop);

    RUN_TEST(test_forget_existing_key_returns_ok);
    RUN_TEST(test_forget_missing_key_returns_ok);
    RUN_TEST(test_forget_null_ssid_rejected);
    RUN_TEST(test_forget_empty_ssid_rejected);
    RUN_TEST(test_forget_does_not_affect_other_ssid);

    RUN_TEST(test_long_password_roundtrip);
}