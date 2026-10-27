// WorkBuddy 状态看板 · 星智 1.54TFT WiFi (ESP32-S3)
//
// 设计说明
// --------
// 本固件只复用星智 1.54TFT 的硬件引脚定义（见 board_config.h），
// 不继承小智 xiaozhi-esp32 的语音链路。原因：
//   小智的 Application 状态机与 LcdDisplay 界面树（top_bar_/content_/emoji_box_）
//   全部围绕语音对话构建。看板是纯数据展示，套进去需要拆掉大半，
//   且对话态会覆盖看板内容。硬件参考 > 代码继承。
//
// 数据流：WiFi -> HTTP GET bridge(/status) -> 解析 JSON -> LVGL 渲染 ST7789
//
// 引脚来源：78/xiaozhi-esp32 main/boards/nologo/xingzhi-cube-1.54tft-wifi/config.h
// 芯片实测：ESP32-S3 QFN56 rev0.2 / 8MB Octal PSRAM / 16MB Flash (esptool flash_id)
//
// ===========================================================================
// 模块分工
// ===========================================================================
//
//   main.cpp          ← 本文件：入口 + LVGL 任务 + 各模块编排
//   board_display.*  屏幕驱动：SPI / ST7789 / LVGL 显示对象 / 背光
//   ui_board.*       看板 UI：控件树 / 渲染 / 切屏 / 休眠 / 浮层
//   data_fetch.*     取数任务：GET → 解析 → 写共享快照
//   board_data.*     数据结构 + 线程安全的共享快照
//   board_json.*     极简 JSON 解析
//   buttons.*        按键消抖与短按/长按判定
//   pet_view.*       像素宠物：建图 / 换帧 / 状态机
//   device_report.*  芯片温度 + 板载指标上报
//   text_util.*      字体自检 + UTF-8 缺字降级
//   wifi_mgr.*       联网 / 配网 / 服务地址配置页
//   audio.*          I2S 提示音
//   battery.*        电量 + 充电状态
//
// 依赖是单向的：main → 各模块；ui_board → board_data / board_display /
// data_fetch / device_report / pet_view / text_util / wifi_mgr；
// data_fetch → board_data / board_json / device_report。
// buttons 不依赖任何业务模块 —— 动作全靠回调注入。

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "lvgl.h"

#include "wifi_mgr.h"
#include "board_data.h"
#include "board_display.h"
#include "buttons.h"
#include "data_fetch.h"
#include "device_report.h"
#include "ui_board.h"
#include "text_util.h"
#include "board_config.h"
#include "battery.h"
#include "audio.h"

static const char *TAG = "wb-board";

// ===========================================================================
// 用户配置
// ===========================================================================
// config.local.h 由 configure.py 生成（不进版本库）。
// 它覆盖下面的默认值，因此改 WiFi 不用动本文件。
#if __has_include("config.local.h")
#include "config.local.h"
#endif

// ⚠ 这里**没有** WIFI_SSID / WIFI_PASSWORD。
// 2026-10-27 起固件只从 NVS 读 WiFi 凭据，没有就开机进配网模式。
// 以前留着的这两个宏已经是死代码（wifi_mgr 不读、configure.py 也不写），
// 留着只会让下一个改代码的人以为"改这里就能换 WiFi"—— 改了完全没效果。
//
// 服务地址（本机以太网 IP，设备必须连同一个路由器）
// 只给 base：第 0 屏走 /status，其余屏走 /screen/<i>，路径由 s_page 决定
// 端口 27777 与服务端 DEFAULT_PORT 保持一致（刻意避开 8080）
//
// ⚠ 这只是**出厂兜底值**。2026-10-27 起地址存在 NVS 里，配过一次就以 NVS
// 为准：电脑换 IP 时在配置页改一下就行，不用改这里、也不用重烧。
// 配置页地址见屏幕上的"服务连不上"提示（= 设备自己的 IP）。
#ifndef BRIDGE_BASE
#define BRIDGE_BASE       "http://192.168.0.89:27777"
#endif
/* 轮询周期、/status 路径都在 data_fetch.cpp —— 它们只服务取数任务。 */

// ===========================================================================
// WiFi
//
// 连接、重试、配网全部收敛到 wifi_mgr.cpp。这里只保留两件事：
//   · 启动时调一次 wifi_mgr_begin()
//   · 屏幕按 wifi_mgr_state() 显示对应提示
//
// 为什么从原文件搬走：原来 wifi_handler 在 STA_DISCONNECTED 回调里
// vTaskDelay(2000) 再重连，会把默认事件循环任务卡住（单线程），
// 期间 WiFi/IP 的所有事件都出不来。新实现改用 esp_timer 调度。
// ===========================================================================

/* ⚠ 这里原来配 SNTP + TZ，是一段死代码，已删除。
 *
 * 屏幕上的 clock / date 全部来自服务端（api_server.py 里 datetime.now()
 * 格式化后下发），固件从头到尾没有调用过 localtime / strftime。
 * 也就是说：SNTP 对齐的时间戳，**没有任何代码去读**。
 *
 * 留着它的代价是实打实的：
 *   · esp_sntp_init() 会持续向公网发 NTP 请求，流量和功耗都是白付；
 *   · 两台服务器轮询，重连 WiFi 后还会重新握手；
 *   · 设备在断开 bridge、根本没网的时候也在对外面发包。
 *
 * 真要用本地时间（比如离线时显示时间），正确做法是加一个
 * localtime() + strftime() 的渲染路径，再把 SNTP 调回来。 */
#ifdef CONFIG_WB_LOCAL_TIME
static void time_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_init();
}
#endif

// ===========================================================================
// 按键任务 → LVGL 任务的请求标志
// ===========================================================================

// 按键写入的配网请求，由 LVGL 任务消费（wifi_mgr_start_provision 里有
// esp_wifi_set_mode / httpd_start，放按键任务里跑会长时间阻塞按键扫描）
static volatile bool s_prov_req = false;

// 按键写入的"重开服务地址配置页"请求，同样由 LVGL 任务执行
// （开 httpd 会阻塞几百毫秒，放按键任务里长按判定会失灵）
static volatile bool s_svrcfg_req = false;

/* ---- 按键动作：按键任务只投递请求，由 LVGL 任务落实 ----
 *
 * 这里刻意只写标志位：LVGL 不是线程安全的，按键任务（优先级 3）如果直接
 * 调 page_apply / lv_label_set_text，会和 lvgl 任务（优先级 4）抢同一个控件树。
 * 原实现就是这个约定，拆出按键扫描后继续保持。 */
static void btn_provision(void)  { s_prov_req   = true; }
static void btn_svr_config(void) { s_svrcfg_req = true; }

static bool btn_wifi_connected(void) { return wifi_mgr_state() == WIFI_ST_CONNECTED; }
static bool btn_svr_down(void)       { return wifi_mgr_svr_down(); }
static bool btn_is_asleep(void)      { return ui_board_asleep(); }

/* 位置初始化而不是指定字段：C++ 下只填部分字段会被 -Wmissing-field-initializers
 * 判成错误（项目开了 -Werror）。顺序必须与 buttons_hooks_t 一致。 */
static const buttons_hooks_t k_btn_hooks = {
    btn_wifi_connected,
    btn_svr_down,
    btn_is_asleep,
    ui_board_wake,        // 唤醒请求
    ui_board_next_page,   // 切屏请求
    ui_board_brightness,  // 亮度调整
    btn_provision,
    btn_svr_config,
};

// ===========================================================================
// LVGL 任务
// ===========================================================================

static void lvgl_task(void *arg)
{
    while (1) {
        // UI 侧的请求消费（切屏、唤醒、亮度）
        ui_board_poll_input();

        // 配网请求：按键任务只置标志，真正开热点/起 HTTP 服务在这里做。
        // 那两个动作会阻塞几百毫秒，放按键任务里会让长按判定失灵。
        //
        // 顺序要紧：这两个动作会改 wifi_mgr 的状态，所以浮层同步必须排在
        // 它们**之后** —— 否则屏上会晚一帧才显示"配网模式"。
        if (s_prov_req) {
            s_prov_req = false;
            wifi_mgr_start_provision();
        }

        // 重开服务地址配置页
        if (s_svrcfg_req) {
            s_svrcfg_req = false;
            wifi_mgr_start_svr_config();
        }

        // 浮层同步 + 按键提示（必须在上面的网络动作之后）
        ui_board_poll_status();

        // ⚠ board_data_get() 内部只做一次加锁 memcpy，绘制在锁外。
        // 原来这里是"持锁调 ui_render()"，LVGL 绘制全在锁里做完。
        // 绘制要走 SPI、可能几十毫秒，期间 fetch 任务拿不到锁就超时，
        // 那一轮拉到的数据被静默丢弃（还看不出报错，只是屏幕不更新）。
        // 取不到锁（50ms 超时）就跳过这一帧，下一帧 20~50ms 后就来。
        board_data_t snap;
        if (board_data_get(&snap)) ui_board_render(&snap);
        uint32_t wait = lv_timer_handler();
        if (wait < 5)  wait = 5;
        if (wait > 50) wait = 50;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

// ===========================================================================
// 入口
// ===========================================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== WorkBuddy 看板启动 ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    board_data_init();

    // lv_init() 必须在 board_display_init() 之前！
    // board_display_init() 内部要调 lv_display_create / lv_display_set_buffers，
    // 顺序反了 LVGL 内部结构还没初始化就往堆里写，会直接踩坏堆：
    //   assert failed: remove_free_block tlsf_control_functions.h:372
    lv_init();
    lv_tick_set_cb([](void) -> uint32_t {
        return (uint32_t)(esp_timer_get_time() / 1000);
    });

    // 屏幕 + 背光。背光初始化完成后是灭的（duty=0），
    // 等 UI 建好再点亮，避免上电白闪。
    board_display_init();

    font_probe();

    // 电量采样与 WiFi 无关，越早开越好 —— 首帧就能带出真实百分比，
    // 而不是先显示 "--" 再跳变。放在 ui_board_build() 之前即可。
    battery_init();

    ui_board_build();

    // 先渲染一帧再开背光，避免上电白闪
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(300));
    board_display_backlight_set(ui_board_brightness_pct());   // 默认 100%

    buttons_start(&k_btn_hooks);

    // 提示音。放在按键任务之后、wifi 之前：
    // I2S 初始化只依赖 GPIO，与网络无关；早开的好处是第一次
    // "任务完成"事件就能响，不会因为还在连 WiFi 而静音。
    // 失败不影响主流程（内部只打日志、不 abort）。
    audio_init();

    // 温度传感器：装不上不影响看板，只是页脚显示 --
    device_report_init();

    /* 编译期 BRIDGE_BASE 只当"一次都没配过"时的初值。
     * NVS 里配过就以 NVS 为准 —— 电脑换 IP 在配置页改一下就行，不用重烧。 */
    wifi_mgr_set_default_base(BRIDGE_BASE);

    wifi_mgr_begin();
#ifdef CONFIG_WB_LOCAL_TIME
    time_init();
#endif

    /* 取数任务。创建失败会在模块内部打 ERROR —— 那种情况设备能亮屏能连
     * WiFi，只是数据永远不刷新，静默失效，日志里必须有痕迹。
     * 两个入参是"当前屏号"和"最近按键名"的取值函数，每轮现问。 */
    data_fetch_start(ui_board_page, ui_board_last_btn);
    if (xTaskCreate(lvgl_task,   "lvgl",   8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "lvgl 任务创建失败 —— 屏幕不会刷新");
    }
}
