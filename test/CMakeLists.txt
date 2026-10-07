cmake_minimum_required(VERSION 3.16)
project(wifi_svc_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(unity STATIC unity_src/src/unity.c)
target_include_directories(unity PUBLIC unity_src/src)

add_library(mocks STATIC
    mocks/esp_err.c
    mocks/esp_crc.c
    mocks/esp_event.c
    mocks/esp_netif.c
    mocks/esp_wifi.c
    mocks/nvs_flash.c
    mocks/esp_netif_sntp.c
    mocks/freertos/freertos.c
)
target_include_directories(mocks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
find_package(Threads REQUIRED)
target_link_libraries(mocks PUBLIC Threads::Threads)

add_library(wifi_svc_under_test STATIC
    ${CMAKE_SOURCE_DIR}/../main/wifi_svc.c
    ${CMAKE_SOURCE_DIR}/../main/wifi_creds.c
)
target_include_directories(wifi_svc_under_test PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_SOURCE_DIR}/../main
)
target_link_libraries(wifi_svc_under_test PUBLIC mocks)

add_executable(run_tests
    test_main.c
    test_wifi_svc.c
)
target_link_libraries(run_tests unity wifi_svc_under_test)

enable_testing()
add_test(NAME unit_tests COMMAND run_tests)
