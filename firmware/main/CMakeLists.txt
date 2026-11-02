# ---- 板级源文件按芯片型号二选一 ----
#
# 同一份应用代码，两块板各有一套"脏活"驱动：
#   S3（星智 1.54TFT）   : 单颗 BOOT 键 + ADC2 直测电池 + 充电 GPIO
#   C3（AI Passport）    : 三键 ADC 分压 + CW2017 电量计（I2C）
# 应用层（ui_board / wb_*）完全不感知差异，接口都收敛在各自头文件里。
if(IDF_TARGET STREQUAL "esp32c3")
    set(BOARD_BUTTON_SRC "buttons_adc.cpp")
    set(BOARD_BATTERY_SRC "battery_cw2017.cpp")
else()
    set(BOARD_BUTTON_SRC "buttons.cpp")
    set(BOARD_BATTERY_SRC "battery.cpp")
endif()
message(STATUS "板级驱动: ${BOARD_BUTTON_SRC} / ${BOARD_BATTERY_SRC}")

idf_component_register(
    SRCS
        "main.cpp"
        "board_data.cpp"
        "board_display.cpp"
        "board_json.cpp"
        "${BOARD_BUTTON_SRC}"
        "data_fetch.cpp"
        "device_report.cpp"
        "pet_view.cpp"
        "text_util.cpp"
        "ui_board.cpp"
        "audio.cpp"
        "${BOARD_BATTERY_SRC}"
        "power_save.cpp"
        "wifi_mgr.cpp"
        # ---- WorkBuddy 控制台（BLE 伴生控制台）----
        # wb_json/wb_proto/wb_core 是纯 C 逻辑层，与 ai-passport(C3) 版共用同一套
        # host 单测；wb_ble 是 NimBLE GATT；wb_view/wb_console 是这块 S3 板的
        # LVGL 单任务模型适配（看板 ui_board 不受影响，两套 screen 并存）。
        # mic.c / wb_adpcm.c 已随语音上行下线一起移除（2026-10-31）。
        "wb_json.c"
        "wb_proto.c"
        "wb_core.c"
        "wb_ble.c"
        "wb_view.c"
        "wb_eyes.c"
        "wb_console.c"
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
        esp_driver_i2c
        esp_driver_tsens
        esp_adc
        esp_lcd
        nvs_flash
        bt
)

# 只走蓝牙（默认开）。
#
# 这颗板子的数据通路已经整体搬到 BLE：控制台六页、状态帧、语音上行都由
# wb_ble 承担。WiFi 那边只剩下"原看板拉 HTTP 数据"这条现在没人用的旧路，
# 而不启动它有三件实实在在的好处：
#   · 不再开 AP 配网。没有 NVS 凭据时原来开机就开热点 + httpd，屏幕上盖一层
#     "配网模式"，看起来像设备出了问题 —— 而这条板子根本不需要联网配什么；
#   · 省下 esp_wifi_init 的静态缓冲（RX 8 个 / TX 32 个）和 lwIP 那一整套。
#     加了蓝牙 + 语音之后，内部 DRAM 正是最紧的地方；
#   · 射频只剩 BLE，功耗本来就低，省电档位那套（依赖 WiFi PS 模式）不用再跑。
#
# ⚠ 副作用（都是有意的）：
#   · wifi_mgr_state() 恒为 WIFI_ST_CONNECTING → 看板的 WiFi 浮层不显示；
#   · data_fetch 任务不创建 → 看板取不到数（数据走 BLE 了，本来也取不到）；
#   · 控制台改成"开机就占屏"，未连主机时显示离线态，而不是退回一张空看板。
#
# 想回退：cmake -DWB_BLE_ONLY=0
option(WB_BLE_ONLY "只走蓝牙，不启动 WiFi / HTTP 轮询" ON)
if(WB_BLE_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WB_BLE_ONLY)
    message(STATUS "WB_BLE_ONLY: 不启动 WiFi，控制台只走 BLE")
endif()

# C3 三键板：让 wb_console 启用三键语义（短 UP=上一页 / 短 DOWN=下一页 /
# 短 OK=回总览 / 长 DOWN=退出浮层 / 长 OK=亮度）。
# S3 单键板不定义这个宏，handle_key 走原来的单键分支，行为一字不改。
if(IDF_TARGET STREQUAL "esp32c3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WB_KEYS3)
    message(STATUS "WB_KEYS3: 三键语义（UP/DOWN/OK）")
endif()

# 调试钩子：cmake 传 -DWB_PREVIEW_DATA=1 会让控制台用假数据渲染六页界面，
# 用来在没有主机（蓝牙连不上）时核对版式。正式固件不带这个参数。
#
# 用 if(WB_PREVIEW_DATA) 而不是 if(DEFINED ...)：后者只要缓存里存在这个变量
# 就成立，于是 -DWB_PREVIEW_DATA=0 反而也会打开预览 —— 这个坑踩过一次：
# 以为关了预览去烧真机，结果设备还在用假数据，怎么按都进不去正式流程。
if(WB_PREVIEW_DATA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WB_PREVIEW_DATA)
    message(STATUS "WB_PREVIEW: 控制台界面使用假数据渲染（仅用于版式自检）")
endif()

# 测试钩子：cmake 传 -DWB_TEST_IGNORE_BUSY=1 会定义这个宏，
# 让 sleep_update 忽略 working/session，以便在有人干活时也能实测
# 省电三档（否则设备永远不空闲，档位切换没法验证）。
# 正式固件不带这个参数，行为不变。
if(DEFINED WB_TEST_IGNORE_BUSY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WB_TEST_IGNORE_BUSY)
    message(STATUS "WB_TEST: 忽略 busy 判定（仅用于验证省电档位）")
endif()

# 测试钩子：cmake 传 -DWB_TEST_STANDBY_FAST=1 把"空闲多久进待机页"从
# 3 分钟降到 20 秒，用来实测大眼睛待机页（等真实的 3 分钟没法验收）。
# 正式固件不带这个参数，行为不变。
if(DEFINED WB_TEST_STANDBY_FAST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WB_TEST_STANDBY_FAST)
    message(STATUS "WB_TEST: 待机门槛降到 20 秒（仅用于验证待机页）")
endif()

# 调试钩子：cmake 传 -DWB_TEST_BAT_TICK=1 会让电池采样每轮打一行 "tick"，
# 用来核对采样周期（预热 2s → 稳定 30s）。正式固件不带这个参数。
if(DEFINED WB_TEST_BAT_TICK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BAT_DEBUG_TICK)
    message(STATUS "WB_TEST: 打印电池采样节拍（仅用于核对周期）")
endif()
