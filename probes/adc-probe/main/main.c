// 电池电压读取验证 —— 只读 GPIO17，绝不触碰任何功能脚
//
// 依据（上游 78/xiaozhi-esp32 main/boards/nologo/xingzhi-cube-1.54tft-wifi/power_manager.h）：
//   adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_2 };
//   adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_6, &chan_config);
//   => ADC2_CH6 = GPIO17 是电池分压采样脚
//   => GPIO38 为充电检测，高电平 = 充电中
//
// 上游分段表（raw 12bit）：
//   1970→0%  2062→20%  2154→40%  2246→60%  2338→80%  2430→100%
//
// ⚠️ 严禁碰 GPIO7 / GPIO15 / GPIO16（喇叭 I2S DOUT/BCLK/LRCK）
//    以及 GPIO4 / GPIO5 / GPIO6（麦克风 I2S）。
//    之前误给喇叭脚加内部上下拉，功放收到无效数据流，会发出噪音。

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "bat-probe";

#define BAT_ADC_UNIT   ADC_UNIT_2
#define BAT_ADC_CH     ADC_CHANNEL_6     // ADC2_CH6 = GPIO17
#define BAT_GPIO       GPIO_NUM_17
#define CHRG_GPIO      GPIO_NUM_38

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

// 上游分段表：raw -> 百分比
static int raw_to_percent(int raw)
{
    static const struct { uint16_t adc; uint8_t level; } levels[] = {
        { 1970,   0 },
        { 2062,  20 },
        { 2154,  40 },
        { 2246,  60 },
        { 2338,  80 },
        { 2430, 100 },
    };
    const int n = sizeof(levels) / sizeof(levels[0]);

    if (raw < levels[0].adc)     return 0;
    if (raw >= levels[n - 1].adc) return 100;

    for (int i = 0; i < n - 1; i++) {
        if (raw >= levels[i].adc && raw < levels[i + 1].adc) {
            float r = (float)(raw - levels[i].adc) / (levels[i + 1].adc - levels[i].adc);
            return (int)(levels[i].level + r * (levels[i + 1].level - levels[i].level) + 0.5f);
        }
    }
    return 0;
}

// 取 32 次平均，同时给出 raw 与 mV
static void sample(int *raw_out, int *mv_out, int *min_out, int *max_out)
{
    long raw_sum = 0, mv_sum = 0;
    int  n = 0, rmin = 99999, rmax = -1;

    for (int i = 0; i < 32; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CH, &raw) == ESP_OK) {
            raw_sum += raw;
            if (raw < rmin) rmin = raw;
            if (raw > rmax) rmax = raw;
            int mv = 0;
            if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) mv_sum += mv;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!n) { *raw_out = -1; *mv_out = -1; *min_out = -1; *max_out = -1; return; }

    *raw_out = (int)(raw_sum / n);
    *mv_out  = s_cali ? (int)(mv_sum / n) : -1;
    *min_out = rmin;
    *max_out = rmax;
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== 电池电压验证（GPIO17 / ADC2_CH6）=====");
    ESP_LOGI(TAG, "依据上游 power_manager.h：ADC_UNIT_2 + ADC_CHANNEL_6");
    ESP_LOGI(TAG, "上游分段：1970=0%% 2062=20%% 2154=40%% 2246=60%% 2338=80%% 2430=100%%");

    // --- ADC 初始化 ---
    adc_oneshot_unit_init_cfg_t u = { .unit_id = BAT_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));

    adc_oneshot_chan_cfg_t cc = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BAT_ADC_CH, &cc));

    adc_cali_curve_fitting_config_t c = {
        .unit_id = BAT_ADC_UNIT, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (adc_cali_create_scheme_curve_fitting(&c, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "ADC 校准不可用，只输出 raw 值");
    }

    // --- 充电检测脚（GPIO38，输入，不加上下拉，与上游一致）---
    gpio_config_t chrg = {
        .pin_bit_mask = 1ULL << CHRG_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&chrg));

    ESP_LOGI(TAG, "开始监测：每 2s 一次。拔插 USB 可对比数值变化。");

    int tick = 0;
    while (1) {
        int raw, mv, rmin, rmax;
        sample(&raw, &mv, &rmin, &rmax);
        int pct  = raw_to_percent(raw);
        int chrg_lv = gpio_get_level(CHRG_GPIO);

        // 跨度大说明采样不稳（可能有噪声或分压电路异常）
        int span = rmax - rmin;

        ESP_LOGI(TAG, "[%3ds] GPIO17: raw=%4d (跨度%3d)  %4dmV  => %3d%%  | CHRG=%d %s",
                 tick, raw, span, mv, pct, chrg_lv,
                 chrg_lv ? "充电中" : "放电/未充电");

        tick += 2;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
