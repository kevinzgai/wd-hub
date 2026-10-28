idf_component_register(
    SRCS
        "main.cpp"
        "board_data.cpp"
        "board_display.cpp"
        "board_json.cpp"
        "buttons.cpp"
        "data_fetch.cpp"
        "device_report.cpp"
        "pet_view.cpp"
        "text_util.cpp"
        "ui_board.cpp"
        "audio.cpp"
        "battery.cpp"
        "power_save.cpp"
        "wifi_mgr.cpp"
        "font_cn_12.c"
        "font_cn_16.c"
        "font_cn_28.c"
        "font_cn_72.c"
        "logo_wb.c"
        "pet_frames.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_http_client
        esp_http_server
        esp_event
        esp_timer
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2s
        esp_driver_tsens
        esp_adc
        esp_lcd
        nvs_flash
)

# 测试钩子：cmake 传 -DWB_TEST_IGNORE_BUSY=1 会定义这个宏，
# 让 sleep_update 忽略 working/session，以便在有人干活时也能实测
# 省电三档（否则设备永远不空闲，档位切换没法验证）。
# 正式固件不带这个参数，行为不变。
if(DEFINED WB_TEST_IGNORE_BUSY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WB_TEST_IGNORE_BUSY)
    message(STATUS "WB_TEST: 忽略 busy 判定（仅用于验证省电档位）")
endif()

# 调试钩子：cmake 传 -DWB_TEST_BAT_TICK=1 会让电池采样每轮打一行 "tick"，
# 用来核对采样周期（预热 2s → 稳定 30s）。正式固件不带这个参数。
if(DEFINED WB_TEST_BAT_TICK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BAT_DEBUG_TICK)
    message(STATUS "WB_TEST: 打印电池采样节拍（仅用于核对周期）")
endif()
