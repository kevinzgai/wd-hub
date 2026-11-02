// 三键 ADC 扫描 —— ESP32-C3（上千 AI Passport）专用实现。
//
// 与 buttons.cpp 的关系：两者对外接口完全相同（buttons_start），
// 由 CMake 按芯片型号二选一编译。区别只在"怎么读出按键"：
//   buttons.cpp   —— 每个键一个 GPIO，低电平有效（S3 板只有 BOOT 一颗）
//   buttons_adc.cpp —— 三键共用 GPIO0 / ADC1_CH0，靠分压电阻区分电压档
//
// 阈值与长按时间来自 passport-os 仓库 components/bsp/include/bsp_pins.h
// (MIT License, HEAD b4d9555)：
//   UP   0~150mV / DOWN 150~450mV / OK 450~1000mV / 松开 ≥2500mV
// 本文件按我们现有事件语义（buttons_hooks_t）重新做了手势映射：
//
//   事件              | 控制台占屏（WB）                  | 看板页
//   ------------------|-----------------------------------|--------------------------
//   短按 UP           | 上一页                            | 上一屏
//   短按 DOWN         | 下一页                            | 下一屏
//   短按 OK           | 唤醒/回总览                        | 唤醒/回总览
//   长按 DOWN         | 返回首页/退出当前浮层              | 回总览
//   长按 OK           | 亮度调节（保留原长按调亮语义）      | 亮度调节
//   长按 UP           | 配网/配置页自救入口                 | 配网/配置页自救入口
//   待机（省电/大眼睛）| 任意键唤醒并恢复 100% 背光          | 任意键唤醒并恢复背光
//
// 取舍说明（与仓库语义的冲突点）：
//   · 仓库把 UP 长按当语音键（800ms 起录）。我们语音上行已下线，这个手势
//     空出来给"配网自救"，与现有长按 3 秒进配网的语义合并为一条。
//   · 仓库的 OK 长按是 2s/5s 关机。我们要求保留"长按调亮度"这一已验收行为，
//     因此关机不做（这块板子也没有 PMIC 电源锁存）。
//   · 审批页/提问页仍按原来的短按允许、长按拒绝 / 短按移选项、长按提交，
//     不套用导航页的 3 键映射 —— 这两个页面少了任何一个结果都会卡住主机。

#include "buttons.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "board_config.h"
#include "audio.h"          // 按下确认时的反馈音（AUDIO_EV_KEY）

static const char *TAG = "keys";

// 面板上电瞬态抑制窗口：SLPOUT + 120ms + DISPON + 首帧全屏刷新 ≈ 300ms，
// 取 600ms。这段时间内 SPI 活动会耦合进 ADC1_CH0，读数不可信。
#define BTN_GLITCH_US   (600 * 1000)

// 与 buttons.h 的 BTN_LONG_MS 不同：仓库实测 800ms 的手感更接近这块板，
// 且用户明确指定 800ms。改这里只影响 C3。
#define BTN_ADC_LONG_MS  800

static const buttons_hooks_t *s_hooks = NULL;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int64_t                   s_ignore_until_us;

// 键序号（与 s_key_name 下标一致）。-1 = 没有键被按住。
enum { KEY_UP = 0, KEY_DOWN = 1, KEY_OK = 2, KEY_NONE = -1 };

static const char *s_key_name[3] = { "UP", "DOWN", "OK" };

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------
static bool adc_begin(void)
{
    adc_oneshot_unit_init_cfg_t ucfg;
    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.unit_id = BOARD_BTN_ADC_UNIT;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit 创建失败");
        s_adc = NULL;
        return false;
    }

    adc_oneshot_chan_cfg_t ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.atten    = BOARD_BTN_ADC_ATTEN;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_adc, BOARD_BTN_ADC_CH, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败");
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return false;
    }

    // 校准必须和按键判定用同一个衰减档，否则读数会和阈值表错位。
    adc_cali_curve_fitting_config_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.unit_id  = BOARD_BTN_ADC_UNIT;
    cal.chan     = BOARD_BTN_ADC_CH;
    cal.atten    = BOARD_BTN_ADC_ATTEN;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        // 没有校准就只能拿 raw 猜电压，阈值表会整体漂 —— 直接判为不可用。
        ESP_LOGE(TAG, "ADC 校准创建失败（三键不可用）");
        s_cali = NULL;
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return false;
    }

    ESP_LOGI(TAG, "按键就绪: ADC1_CH%d 三键分压（UP 0-%dmV / DOWN %d-%dmV / OK %d-%dmV）",
             BOARD_BTN_ADC_CH,
             BOARD_BTN_MV_UP_MAX,
             BOARD_BTN_MV_DOWN_MIN, BOARD_BTN_MV_DOWN_MAX,
             BOARD_BTN_MV_OK_MIN, BOARD_BTN_MV_OK_MAX);
    return true;
}

// 读一次电压并归类。返回 KEY_NONE 表示松开（或读数无效）。
static int read_key(void)
{
    if (!s_adc || !s_cali) return KEY_NONE;
    if (esp_timer_get_time() < s_ignore_until_us) return KEY_NONE;

    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BOARD_BTN_ADC_CH, &raw) != ESP_OK) return KEY_NONE;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return KEY_NONE;

    // 1000~2500mV 是死区：既不是有效按键，也没到松开门槛。
    // 按"未按键"处理，避免手松开途中被误判成某个键。
    if (mv < BOARD_BTN_MV_UP_MAX)                                 return KEY_UP;
    if (mv < BOARD_BTN_MV_DOWN_MAX)                               return KEY_DOWN;
    if (mv < BOARD_BTN_MV_OK_MAX)                                 return KEY_OK;
    return KEY_NONE;
}

// ---------------------------------------------------------------------------
// 语义分派
// ---------------------------------------------------------------------------
static bool asleep(void)
{
    return s_hooks && s_hooks->is_asleep && s_hooks->is_asleep();
}

static bool console_active(void)
{
    return s_hooks && s_hooks->console_active && s_hooks->console_active();
}

static void do_provision(void)
{
    ESP_LOGW(TAG, ">>> UP 长按，进入配网/配置页自救");
    if (s_hooks && s_hooks->provision) s_hooks->provision();
}

// 短按松手时调用。此时已经确认"没按满长按阈值"。
static void on_short(int key)
{
    // 待机/省电：任意键只负责唤醒，不顺带翻页 ——
    // 屏幕黑着的时候用户按一下的唯一意图是"我想看屏幕"。
    if (asleep()) {
        if (s_hooks && s_hooks->wake) s_hooks->wake();
        return;
    }

    if (console_active()) {
        if (s_hooks && s_hooks->wake) s_hooks->wake();
        if (s_hooks && s_hooks->console_key) {
            s_hooks->console_key(s_key_name[key], false);
        }
        return;
    }

    switch (key) {
    case KEY_UP:
        if (s_hooks && s_hooks->prev_page) s_hooks->prev_page();
        break;
    case KEY_DOWN:
        if (s_hooks && s_hooks->next_page) s_hooks->next_page();
        break;
    case KEY_OK:
    default:
        if (s_hooks && s_hooks->home) s_hooks->home();
        break;
    }
    // 醒着时也投一个唤醒请求：清掉空闲计时，连按翻屏期间不会中途黑掉。
    if (s_hooks && s_hooks->wake) s_hooks->wake();
}

// 按住够久时调用一次（不重复触发）。
static void on_long(int key)
{
    // 省电/待机：长按同样只当"叫醒"，不顺带调亮度或翻页。
    // 理由：屏幕只有一个时钟时，手指比 800ms 慢一点很正常，
    // 顺带把亮度从 100% 循环掉会让用户莫名其妙。
    if (asleep()) {
        if (s_hooks && s_hooks->wake) s_hooks->wake();
        return;
    }

    if (key == KEY_UP) {
        // 配网自救：不看是否控制台占屏 —— 这正是"屏幕卡在某页出不来"时的入口。
        do_provision();
        return;
    }

    if (console_active()) {
        // 交给控制台：导航页长按 DOWN = 回总览/退出浮层；
        // 长按 OK = 亮度（在 wb_console 里调 ui_board_brightness）。
        // 审批/提问页的语义由 wb_console 自己保留（允许/拒绝、提交）。
        if (s_hooks && s_hooks->console_key) {
            s_hooks->console_key(s_key_name[key], true);
        }
        return;
    }

    if (s_hooks && s_hooks->wake) s_hooks->wake();
    if (key == KEY_DOWN) {
        if (s_hooks && s_hooks->home) s_hooks->home();
    } else {  // KEY_OK：亮度循环（沿用 S3 单键板长按调亮的语义）
        if (s_hooks && s_hooks->brightness) s_hooks->brightness(s_key_name[key], 0);
    }
}

// ---------------------------------------------------------------------------
// 扫描任务
// ---------------------------------------------------------------------------
static void button_task(void *arg)
{
    // 消抖：同一个键连续 NEED 次才确认按下；松开方向要连续 REL 次。
    // 20ms 一轮 → 按下确认 100ms、松开确认 200ms，与 buttons.cpp 一致。
    const int NEED = 5;
    const int REL  = 10;

    int      held      = KEY_NONE;   // 已确认按住的键
    int      cand      = KEY_NONE;   // 正在消抖的候选
    int      cand_cnt  = 0;
    uint32_t down_at   = 0;
    bool     fired     = false;      // 本次按压是否已按长按处理过

    while (1) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const int k = read_key();

        if (k == cand) {
            if (cand_cnt < 100) cand_cnt++;
        } else {
            cand     = k;
            cand_cnt = 0;
        }

        if (held == KEY_NONE) {
            // 空闲 → 按下：候选键稳定 NEED 次才认。
            if (cand != KEY_NONE && cand_cnt >= NEED) {
                held     = cand;
                down_at  = now_ms;
                fired    = false;
                /* 反馈音打在"按下确认"这一刻，不是松手时 ——
                 * 所有动作都要等松手或按满 800ms 才知道，声音挂松手会像延迟。 */
                audio_play(AUDIO_EV_KEY);
            }
        } else {
            // 按住中：先判长按（不等松手，否则按 2 秒才调一次亮度，手感很差）
            if (!fired && (now_ms - down_at) >= BTN_ADC_LONG_MS) {
                fired = true;
                on_long(held);
            }
            // 再判松开：必须回到"无键"状态并稳定 REL 次。
            if (cand == KEY_NONE && cand_cnt >= REL) {
                if (!fired) on_short(held);
                held = KEY_NONE;
                fired = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void buttons_start(const buttons_hooks_t *hooks)
{
    s_hooks = hooks;
    // 显示刚初始化完，先屏蔽一段 ADC 读数。
    s_ignore_until_us = esp_timer_get_time() + BTN_GLITCH_US;

    if (!adc_begin()) {
        ESP_LOGE(TAG, "按键 ADC 不可用 —— 三键将无响应");
        return;
    }

    if (xTaskCreate(button_task, "button", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败 —— 按键将无响应");
    }
}
