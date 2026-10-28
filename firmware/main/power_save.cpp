/* 省电策略 —— 实现
 *
 * 设计取舍写在 power_save.h 里（为什么不用 esp_pm、为什么三级），
 * 这里只记实现上容易踩的坑。
 */

#include "power_save.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_wifi.h"

#include "board_display.h"
#include "wifi_mgr.h"

static const char *TAG = "power";

/* ---- 各级参数。改省电效果就改这张表，别去各模块里改常量 ---- */
#define LIGHT_BRIGHT_PCT   30
#define DEEP_BRIGHT_PCT     5
#define POLL_NORMAL_MS   (5 * 1000)
#define POLL_DEEP_MS    (60 * 1000)
#define LVGL_WAIT_ACTIVE_MS   50
#define LVGL_WAIT_LIGHT_MS   100
#define LVGL_WAIT_DEEP_MS    500

static volatile power_level_t s_level = POWER_ACTIVE;

/* WiFi 省电模式是否已成功下发过。
 *
 * ⚠ 必须在**连上 WiFi 之后**才能设 PS 模式。开机早期调
 * esp_wifi_set_ps() 会因为驱动还没 start 而返回 ESP_ERR_WIFI_NOT_INIT
 * —— 那不是错误，只是时机不到。这个标志就是用来避免每次切级别都
 * 刷一遍失败日志。
 */
static bool s_ps_ready = false;

void power_save_init(void)
{
    /* 开机就是 ACTIVE，但**要显式下发一次** WiFi PS。
     *
     * 原因：IDF 默认是 MIN_MODEM，而我们要的是"醒着的时候别省" ——
     * MIN_MODEM 会让下行包延迟 100ms 左右才收，看板 5 秒一轮、
     * 每轮等 5 秒超时，这个延迟不是问题；但它会让"切屏立刻重拉"
     * 慢一拍。醒着时用 NONE 更跟手。
     *
     * ⚠ 这里必须先把 s_level 设成"非法值"再调 set() ——
     * set() 开头有幂等判断（同一级别直接返回），如果先赋值成 ACTIVE
     * 再调 set(ACTIVE)，会被判成"没变化"直接跳过，WiFi PS 永远不生效。
     */
    s_level = (power_level_t)-1;    /* 故意用非法值 */
    s_ps_ready = false;
    power_save_set(POWER_ACTIVE);
    ESP_LOGI(TAG, "省电策略就绪（当前 ACTIVE）");
}

static void apply_wifi_ps(power_level_t lv)
{
    /* WiFi 没连上就别设。
     *
     * 两个原因：
     *   1. 驱动还没 start 时 esp_wifi_set_ps() 返回 NOT_INIT/NOT_STARTED，
     *      而且**设早了会被后续 esp_wifi_start() 重置** —— 白设。
     *   2. 更重要的：省电模式的意义是"人不在时省电"。WiFi 都没连上
     *      （正在配网 / 连接中），屏幕正在显示错误提示，这时把射频
     *      降速只会让连上更慢。等连上再说。
     *
     * ACTIVE 也一并跳过：醒着时本来就该用驱动默认值，没必要显式设 NONE。
     * 真要用 NONE 的场合（切屏重拉要跟手）由 LIGHT/DEEP 退出时的
     * 第一次 ACTIVE 覆盖 —— 那时 WiFi 一定连着。
     */
    if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
        return;
    }

    wifi_ps_type_t ps;
    switch (lv) {
        case POWER_DEEP:
            /* ⚠ 必须是 MIN_MODEM，MAX_MODEM 实测不可用。
             *
             * 实测记录（60 秒轮询 + esp_http_client_close 双保险下）：
             *   MAX_MODEM → 每轮必定失败，ESP_ERR_HTTP_FETCH_HEADER
             *               与 ESP_ERR_HTTP_CONNECTION_CLOSED 交替出现，
             *               设备随即被判离线、退出省电，60 秒轮询从没成功过一次
             *   MIN_MODEM → 一次成功（实测间隔 60.5s，数据正常）
             *
             * 为什么 MAX_MODEM 不行：它让射频只在 DTIM 周期醒来听 AP 的
             * 缓存指示，休眠窗口比 beacon 间隔（100ms）长得多。停 60 秒
             * 再发请求时，链路层状态已经和 AP 不同步了 —— 试过请求前后
             * 临时 esp_wifi_set_ps(NONE) 唤醒，**照样失败**。
             * 这不是"短暂没醒"，而是长期深度休眠后重同步要几百 ms 到几秒，
             * 远超 8 秒的 HTTP 超时预算里的合理余量。
             *
             * 代价：MIN_MODEM 空闲时每 100ms 醒一次听 beacon，比 MAX_MODEM
             * 多耗几 mW。但省电的大头是**轮询 60 秒 + 2fps**（这两项才是
             * 数倍的差距），拿几 mW 换 100% 拉取成功率，值。
             *
             * 另一条路是 deep sleep（~20µA），但唤醒要重启重连 3~5 秒，
             * 人还在时体验太差，已经否掉了。 */
            ps = WIFI_PS_MIN_MODEM;
            break;
        case POWER_LIGHT:
            /* 轻度：还是 MIN_MODEM。用户可能随时回来，别让切屏变卡。 */
            ps = WIFI_PS_MIN_MODEM;
            break;
        case POWER_ACTIVE:
        default:
            /* 醒着不省：收了就立刻处理，切屏重拉零延迟。 */
            ps = WIFI_PS_NONE;
            break;
    }

    esp_err_t err = esp_wifi_set_ps(ps);
    if (err == ESP_OK) {
        if (!s_ps_ready) {
            s_ps_ready = true;
            ESP_LOGI(TAG, "WiFi 省电模式已接管（PS=%d）", (int)ps);
        }
    } else if (err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        /* NOT_INIT / NOT_STARTED 是"还没开机连上"，属正常时序，不刷日志。
         * 别的错误要报出来 —— 那说明真的设失败了，省电效果会打折。 */
        ESP_LOGW(TAG, "设置 WiFi PS=%d 失败: %s", (int)ps, esp_err_to_name(err));
    }
}

void power_save_set(power_level_t lv)
{
    if (lv == s_level) return;          /* 幂等 */
    s_level = lv;

    switch (lv) {
        case POWER_DEEP:
            board_display_backlight_set(DEEP_BRIGHT_PCT);
            apply_wifi_ps(lv);
            ESP_LOGI(TAG, ">>> 深度省电：背光 %d%% / 轮询 %ds / 帧率 %dms",
                     DEEP_BRIGHT_PCT, POLL_DEEP_MS / 1000, LVGL_WAIT_DEEP_MS);
            break;

        case POWER_LIGHT:
            board_display_backlight_set(LIGHT_BRIGHT_PCT);
            apply_wifi_ps(lv);
            ESP_LOGI(TAG, ">>> 轻度省电：背光 %d%% / 轮询 %ds",
                     LIGHT_BRIGHT_PCT, POLL_NORMAL_MS / 1000);
            break;

        case POWER_ACTIVE:
        default:
            /* 背光**不在这里恢复** —— 恢复正常亮度是 UI 侧的事：
             * 用户按过亮度键的话，得回到他选的档位（40/70/100），
             * 而不是固定值。这里只把 WiFi 和节奏放开。 */
            apply_wifi_ps(POWER_ACTIVE);
            ESP_LOGI(TAG, ">>> 恢复正常功耗");
            break;
    }
}

void power_save_tick(void)
{
    /* WiFi 连上之后补设一次 PS。
     *
     * s_ps_ready 是"成功下发过一次"的标志，设过一次就不再重复 ——
     * 这个函数每帧都跑，不做守卫的话会变成每帧一次 ioctl。
     *
     * 为什么不能只在 power_save_set 里做：开机时 wifi_mgr_begin() 是
     * 异步的，power_save_init() 那一刻 WiFi 肯定还没连上，那次设置被
     * apply_wifi_ps 的守卫挡掉。之后如果一直是 ACTIVE（有任务在跑），
     * 就再没有"级别变化"的时机去补设，WiFi 会一直跑驱动默认值。
     */
    if (!s_ps_ready && wifi_mgr_state() == WIFI_ST_CONNECTED) {
        apply_wifi_ps(s_level);
    }
}

void power_save_http_begin(void)
{
    /* 目前**什么都不做**，保留这对外壳是为了以后换 PS 策略时有个
     * 统一的插入点。
     *
     * 历史：DEEP 档原用 MAX_MODEM，60 秒轮询下请求必失败
     * （ESP_ERR_HTTP_FETCH_HEADER）。试过在这里临时切 WIFI_PS_NONE
     * 唤醒射频 —— 无效。最终改成 MIN_MODEM 解决，就不再需要临时唤醒了。
     *
     * 详见 power_save.cpp 里 POWER_DEEP 分支的说明。 */
}

void power_save_http_end(void)
{
    /* 同 power_save_http_begin。 */
}

power_level_t power_save_level(void)
{
    return s_level;
}

uint32_t power_save_poll_ms(void)
{
    return (s_level == POWER_DEEP) ? POLL_DEEP_MS : POLL_NORMAL_MS;
}

uint32_t power_save_lvgl_max_wait_ms(void)
{
    switch (s_level) {
        case POWER_DEEP:  return LVGL_WAIT_DEEP_MS;
        case POWER_LIGHT: return LVGL_WAIT_LIGHT_MS;
        default:          return LVGL_WAIT_ACTIVE_MS;
    }
}

uint32_t power_save_backlight_pct(void)
{
    switch (s_level) {
        case POWER_DEEP:  return DEEP_BRIGHT_PCT;
        case POWER_LIGHT: return LIGHT_BRIGHT_PCT;
        default:          return 100;
    }
}
