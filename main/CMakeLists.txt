idf_component_register(SRCS 
    "main.c" 
    "bsp_display.c" 
    "wifi_svc.c" 
    "wifi_creds.c"
    "scd4x_svc.c" 
    "ui.c" 
    "rtc_svc.c"
    "fonts/DroidSansMono_128.c" 
    "fonts/Moscow2024_96.c" 
    "fonts/RobotoMono_88.c" 
    "fonts/RobotoMono_80.c"
    INCLUDE_DIRS "." "fonts")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)

