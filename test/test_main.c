#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "esp_err.h"
#include "esp_crc.h"

void setUp(void) {}
void tearDown(void) {}

void run_wifi_svc_tests(void);
void run_wifi_creds_tests(void);

static void test_unity_works(void)
{
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

static void test_crc32_known_vector(void)
{
    const char *s = "123456789";
    uint32_t crc = esp_crc32_le(0, (const uint8_t *)s, strlen(s));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unity_works);
    RUN_TEST(test_crc32_known_vector);
    run_wifi_svc_tests();
    run_wifi_creds_tests();
    return UNITY_END();
}
