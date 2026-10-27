/* 按键扫描 —— 实现（从 main.cpp 搬出，判定逻辑一字未改） */
#include "buttons.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "board_config.h"

static const char *TAG = "keys";

typedef struct {
    gpio_num_t  pin;
    const char *name;
} btn_cfg_t;

static const btn_cfg_t k_btns[] = {
    { BOARD_BTN_BOOT,     "BOOT(GPIO0)"     },
    { BOARD_BTN_VOL_UP,   "VOLUP(GPIO40)"   },
    { BOARD_BTN_VOL_DOWN, "VOLDN(GPIO39)"   },
};
#define BTN_NUM (sizeof(k_btns) / sizeof(k_btns[0]))

static const buttons_hooks_t *s_hooks = NULL;

static void button_init(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < BTN_NUM; i++) {
        mask |= 1ULL << k_btns[i].pin;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = mask;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;   // 按下接地 → 低电平
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;    // 用轮询，抖动好处理
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_LOGI(TAG, "按键监听: BOOT=GPIO%d VOLUP=GPIO%d VOLDN=GPIO%d",
             BOARD_BTN_BOOT, BOARD_BTN_VOL_UP, BOARD_BTN_VOL_DOWN);
}

static bool asleep(void)
{
    return s_hooks && s_hooks->is_asleep && s_hooks->is_asleep();
}

static void button_task(void *arg)
{
    // 消抖：按下要连续 NEED 次低电平确认；松开后必须稳定 REL 次才接受下一次按下，
    // 否则引脚抖动会把一次按压刷成好几次，亮度乱跳。
    const int NEED = 5;    // 20ms * 5  = 100ms 按下确认
    const int REL  = 10;   // 20ms * 10 = 200ms 松开确认
    // 短按 = 切屏，长按 = 调亮度。按下那一刻不知道用户要干嘛，
    // 所以动作只能等到"按住够久"或"松开"时才分派。
    const uint32_t LONG_MS = 700;

    // WiFi 没连上时，BOOT 的长按改语义：按住 3 秒进配网。
    // 这个状态下**不触发** 700ms 的亮度循环 —— 否则一次按住会先跳一档亮度
    // 再进配网，用户会困惑"我明明只想配网，怎么屏幕变亮了"。
    const uint32_t PROV_MS = 3000;

    bool     last[BTN_NUM];
    int      cnt[BTN_NUM]     = {0};
    int      rel[BTN_NUM];
    uint32_t down_at[BTN_NUM] = {0};
    bool     fired[BTN_NUM]   = {false};   // 本次按压是否已按长按处理过

    for (size_t i = 0; i < BTN_NUM; i++) {
        last[i] = gpio_get_level(k_btns[i].pin) != 0;
        // 初始**必须**当作"还没松开"（rel=0），不能直接给 REL。
        //
        // 踩过的坑：给 REL 的话，开机第一次按压无条件被接受。而 GPIO0 是
        // ESP32-S3 的启动 strapping 脚，插拔 USB / 开串口时自动复位电路会
        // 把它拉低一下 —— 实测开机 3.5s 就误触发了一次"BOOT 长按调亮度"。
        // 现在的规则是：必须先看到引脚稳定高电平 REL 次（= 确认松开了），
        // 之后的高→低才算一次有效按压。误触发就被挡在外面了。
        rel[i]  = 0;
    }

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // 每轮取一次即可：状态由 wifi_mgr 的事件回调更新，轮询频率 50Hz 够跟
        bool wifi_ok = (s_hooks && s_hooks->wifi_connected)
                       ? s_hooks->wifi_connected() : false;

        for (size_t i = 0; i < BTN_NUM; i++) {
            bool now = gpio_get_level(k_btns[i].pin) != 0;

            if (now == last[i]) {
                cnt[i] = 0;
                if (now && rel[i] < REL) rel[i]++;   // 持续高电平 → 累计松开时间

                // 按住不放且已经够久 → 立刻按长按处理一次。
                // 不等松手，否则按 3 秒才调一次亮度，手感很差。
                if (!now && !fired[i] && down_at[i]) {
                    uint32_t held = now_ms - down_at[i];
                    bool is_boot = (k_btns[i].pin == BOARD_BTN_BOOT);

                    if (is_boot && !wifi_ok) {
                        // **只有没联网时**，BOOT 长按才让给配网。
                        //
                        // 已联网时 BOOT 保持原语义（700ms 调亮度），按住多久都
                        // 不会进配网 —— 配网会中断数据轮询，联网状态下不应该
                        // 被误触。要换网络就先断网（或重新配网后重启）。
                        //
                        // 未联网时 700ms 什么都不做：那个状态下用户按 3 秒一定
                        // 是想配网，先跳一档亮度只会让人困惑。
                        if (held >= PROV_MS) {
                            fired[i] = true;
                            ESP_LOGW(TAG, ">>> BOOT 长按 %lums（未联网），进入配网",
                                     (unsigned long)held);
                            if (s_hooks && s_hooks->provision) s_hooks->provision();
                        }
                    } else if (is_boot && s_hooks && s_hooks->svr_down && s_hooks->svr_down()) {
                        /* WiFi 是通的、但服务连不上：长按 BOOT 3 秒重新开放
                         * 配置页。正常情况页面在连不上时已经自己开了，
                         * 这里是给"手滑关掉页面 / 想再看一眼地址"留的入口。
                         * 同样不触发 700ms 的亮度循环，理由同上。 */
                        if (held >= PROV_MS) {
                            fired[i] = true;
                            ESP_LOGW(TAG, ">>> BOOT 长按 %lums（服务连不上），重开配置页",
                                     (unsigned long)held);
                            if (s_hooks && s_hooks->svr_config) s_hooks->svr_config();
                        }
                    } else if (held >= LONG_MS) {
                        fired[i] = true;
                        // 省电模式下长按 BOOT 只当"退出省电"，不调亮度。
                        //
                        // 理由：屏幕上只有一个时钟，此时按住不放的用户
                        // 十有八九是想把屏幕点回来（"按一下退出"），
                        // 手指比 700ms 慢一点很正常。如果这时顺带把亮度
                        // 从 100% 循环掉到 40%，用户会莫名其妙发现屏幕变暗，
                        // 而且因为 fired 已经被置位，他松手时也不会再翻屏。
                        // VOLUP/VOLDN 在省电下仍然调亮度 —— 那两键唯一语义
                        // 就是调亮度，不存在歧义。
                        bool vol_key = (k_btns[i].pin == BOARD_BTN_VOL_UP) ||
                                       (k_btns[i].pin == BOARD_BTN_VOL_DOWN);
                        if (asleep() && !vol_key) {
                            if (s_hooks && s_hooks->wake) s_hooks->wake();
                        } else {
                            // 调亮度前先唤醒：省电模式是 30% 背光，
                            // 在这个状态下按"调亮"却看不到屏幕变亮，
                            // 用户只会以为按键没反应。
                            // VOL 键也退出省电（用户明确要求）：按 VOL
                            // 这个动作本身就说明"我想看清屏幕"。
                            if (s_hooks && s_hooks->wake) s_hooks->wake();
                            int step = (k_btns[i].pin == BOARD_BTN_VOL_UP)   ? +1
                                     : (k_btns[i].pin == BOARD_BTN_VOL_DOWN) ? -1
                                     : 0;
                            if (s_hooks && s_hooks->brightness) {
                                s_hooks->brightness(k_btns[i].name, step);
                            }
                        }
                    }
                }
                continue;
            }
            if (++cnt[i] < NEED) continue;

            cnt[i]  = 0;
            last[i] = now;

            if (!now) {                              // 高→低 = 按下
                if (rel[i] >= REL) {                 // 之前确实松开了才算一次有效按压
                    down_at[i] = now_ms;
                    fired[i]   = false;
                }
                rel[i] = 0;
            } else {                                 // 低→高 = 松开
                // 短按（没走长按分支）→ 下一屏。
                // **不能直接调 page_apply**：它要动控件，而 LVGL 不是线程安全的，
                // 这里只投递请求，由 lvgl_task 在持有 LVGL 上下文时执行。
                //
                // 省电模式下短按只负责"点亮屏幕"，**不顺带切屏**：
                // 屏幕全黑时按键，用户唯一意图是"我想看时间"。
                // 如果这时还切到下一屏，醒来第一眼看到的是详情页而不是
                // 概览页，反而找不到时间 —— 所以他得按第二下才能回去。
                // 醒来后再按，才按正常语义翻屏。
                if (down_at[i] && !fired[i]) {
                    if (asleep()) {
                        if (s_hooks && s_hooks->wake) s_hooks->wake();
                    } else {
                        if (s_hooks && s_hooks->next_page) s_hooks->next_page();
                        // 醒着时也投一个唤醒请求：sleep_apply(false) 是空操作
                        // （内部有 on == s_asleep 的判等），但会把空闲计时清零，
                        // 这样连按几下翻屏的期间不会中途黑掉。
                        if (s_hooks && s_hooks->wake) s_hooks->wake();
                    }
                }
                down_at[i] = 0;
                fired[i]   = false;
                rel[i]     = 0;                      // 刚松开，重新计时
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void buttons_start(const buttons_hooks_t *hooks)
{
    s_hooks = hooks;
    button_init();

    if (xTaskCreate(button_task, "button", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败 —— 按键将无响应");
    }
}
