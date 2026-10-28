// 电池电量读取实现 —— 详见 battery.h 顶部的引脚依据说明

#include "battery.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "board_config.h"

static const char *TAG = "battery";

// ADC_UNIT_2 = 1、ADC_CHANNEL_6 = 6（枚举值，不是 GPIO 号）。
// GPIO17 与 ADC2_CH6 的对应关系见 soc/esp32s3/adc_channel.h:
//   #define ADC2_CHANNEL_6_GPIO_NUM 17
// 这里直接用枚举名而不是在 board_config.h 里做数值换算是刻意的 ——
// 数值换算需要假定 enum 从 0 连续递增，换成枚举名编译器会替我校验。
#define BAT_ADC_UNIT   ADC_UNIT_2
#define BAT_ADC_CH     ADC_CHANNEL_6

// 采样周期。电量是慢变量，30 秒一次足够，也顺手把 DEEP 档的功耗缺口
// 补上 —— 原来 2 秒一次（每轮还是 16 次 oversample + 5ms 间隔 ≈ 80ms
// ADC 常开），整机都降到 60 秒轮询了，这里每 2 秒醒一次采 ADC 很讽刺。
#define BAT_SAMPLE_PERIOD_MS   30000

// 还没拿到过有效读数时的重试周期。
//
// ⚠ 这个不能省。充电状态要去抖（连续 CHRG_DEBOUNCE_N 次同向才认），
// 而 chrg_settled 之前 bat_sample_once 直接 return，不会写 s_state ——
// 一律 30 秒的话，开机要 3 × 30 = 90 秒才出第一个电量值，屏幕上
// 电池圈空一分半。用 s_state.valid 当判据快速重试，开机 6 秒内出数。
#define BAT_WARMUP_PERIOD_MS   2000

#define BAT_OVERSAMPLE         16      // 单次平均的样本数
#define BAT_OVERSAMPLE_GAP_MS  5

// 充电压差（端电压 - OCV）的上限，仅给"插着电开机、拿不到放电快照"的
// 兜底分支当最低一档用。压差本身随电量变化很大：
//   满电 CV 末尾 电流趋零 -> 压差≈0，端电压≈4.19V（raw≈2440+）
//   CC 中段               -> 压差最大，可达 0.3V（raw≈180）
// 所以真正的分档表写在 bat_sample_once 里，这里只留一个上界常量。
#define CC_CV_RAW_DROP  180

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static SemaphoreHandle_t         s_lock;

static battery_state_t s_state = {
    .raw = -1, .mv = -1, .percent = -1, .volt_pct = -1,
    .charging = false, .percent_held = false, .valid = false,
};

// 充电前的最后可信电量。充电中拿它当显示值，避免跳到 100%。
static int  s_last_discharge_pct = -1;

// 充电状态去抖。
// 为什么必须去抖：开机复位瞬间 GPIO38 还没稳定，前几次采样可能读成 0（"没充电"）。
// 那时按电压算出来是满电 99~100%，会被当成"充电前快照"存起来 ——
// 结果整个充电过程都显示这个开机猜出来的值（实测就是 99%[保持] 一直不动）。
// 连续 N 次一致才认可状态切换，开机头几秒的误读就进不来。
#define CHRG_DEBOUNCE_N   3
static int  s_discharge_streak = 0;   // 连续读成"放电"的次数
static int  s_charge_streak    = 0;   // 连续读成"充电"的次数
static bool s_charging_stable   = false;  // 去抖后的稳定状态

int battery_raw_to_percent(int raw)
{
    // 上游 power_manager.h 的分段表，原样保留
    static const struct { int adc; int level; } levels[] = {
        { 1970,   0 },
        { 2062,  20 },
        { 2154,  40 },
        { 2246,  60 },
        { 2338,  80 },
        { 2430, 100 },
    };
    const int n = (int)(sizeof(levels) / sizeof(levels[0]));

    if (raw <  levels[0].adc)      return 0;
    if (raw >= levels[n - 1].adc)  return 100;

    for (int i = 0; i < n - 1; i++) {
        if (raw >= levels[i].adc && raw < levels[i + 1].adc) {
            float r = (float)(raw - levels[i].adc) /
                      (float)(levels[i + 1].adc - levels[i].adc);
            int pct = levels[i].level +
                      (int)(r * (levels[i + 1].level - levels[i].level) + 0.5f);
            return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
        }
    }
    return 0;
}

// 采一次平均，写入 s_state（调用方需持锁，或独占期调用）
static void bat_sample_once(void)
{
#ifdef BAT_DEBUG_TICK
    /* 临时验证用：每次采样打一行，看周期对不对。
     * 平时不定义，正式固件一行都不打。 */
    ESP_LOGI(TAG, "tick");
#endif
    long raw_sum = 0;
    long mv_sum  = 0;
    int  n = 0;
    int  n_mv = 0;      // ⚠ 校准成功的次数，和 n 不一定相等

    for (int i = 0; i < BAT_OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CH, &raw) == ESP_OK) {
            raw_sum += raw;
            int mv = 0;
            if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
                mv_sum += mv;
                n_mv++;
            }
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(BAT_OVERSAMPLE_GAP_MS));
    }
    if (!n) return;   // 全失败就不更新，保留上次值

    int raw_avg = (int)(raw_sum / n);
    /* ⚠ 除数必须用 n_mv，不能用 n。
     * n 数的是「ADC 读成功」次数，mv_sum 只在「校准转换成功」时累加。
     * 两者不等时（校准表缺项、某些 raw 超出校准范围），
     * 用 n 当除数会把平均值算低 —— 电压读数偏低会直接影响充电判断。 */
    int mv_avg  = (s_cali && n_mv) ? (int)(mv_sum / n_mv) : -1;

    // 得出明显不可能的 raw（0 或满量程）视为异常，不更新百分比。
    if (raw_avg <= 0 || raw_avg >= 4095) {
        ESP_LOGW(TAG, "ADC raw=%d 异常，跳过本次", raw_avg);
        return;
    }

    // 充电状态在这里一并读，因为下面的百分比策略依赖它
    bool chrg_raw = (gpio_get_level(BOARD_CHRG_GPIO) == 1);

    // 去抖：见 s_charging_stable 处的说明
    if (chrg_raw) { s_charge_streak++;    s_discharge_streak = 0; }
    else          { s_discharge_streak++; s_charge_streak    = 0; }

    if (!s_charging_stable && s_charge_streak    >= CHRG_DEBOUNCE_N)
        s_charging_stable = true;
    if ( s_charging_stable && s_discharge_streak >= CHRG_DEBOUNCE_N)
        s_charging_stable = false;

    bool charging = s_charging_stable;

    // 充电标志尚未定型：还没攒够 CHRG_DEBOUNCE_N 次"当前方向"的采样。
    //
    // 注意判据必须是 streak >= N，不能写成 streak == 0。
    // 写成 ==0 时，开机第一轮 dstr=1、cstr=0 就会被当成"已定型为放电"，
    // 于是那次误读的满电值照样进快照 —— 等于没修。实测模拟：
    //   streak==0       -> 第 1、2 轮 settled=True，误读全部放过（错误）
    //   streak>=N       -> 第 1~4 轮 settled=False，第 5 轮才放行（正确）
    bool chrg_settled = s_charging_stable ? (s_charge_streak    >= CHRG_DEBOUNCE_N)
                                          : (s_discharge_streak >= CHRG_DEBOUNCE_N);
    if (!chrg_settled) return;

    // 纯电压换算值 —— 充电时会虚高，只留作调试参考
    int volt_pct = battery_raw_to_percent(raw_avg);

    // ---- 三段式策略：把"充电时虚高"挡在显示之外 ----
    int  show_pct;
    bool held;

    if (!charging) {
        // 1) 放电中：读数可信，直接用。
        //    注意这里的 charging 是去抖后的值，所以开机误读不会污染快照。
        show_pct = volt_pct;
        held     = false;
        s_last_discharge_pct = show_pct;   // 存下来，供下次充电时用
    } else if (s_last_discharge_pct >= 0) {
        // 2) 充电中，且知道充电前是多少：保持那个值
        //    充电不会让电量变少，所以不低于充电前快照；
        //    但也不必虚报——就固定在快照值，充满前不动。
        show_pct = s_last_discharge_pct;
        held     = true;
    } else {
        // 3) 充电中，且从未见过放电态（插着电开机）。
        //    没有快照可用，只能从"充电端电压"倒推真实电量。
        //
        //    这里不能用固定压差 (raw - 180)：充电压差跟电量强相关 ——
        //    满电时 IC 已进 CV 阶段、电流趋零，端电压几乎等于 OCV（压差≈0）；
        //    中段才是 CC 恒流，压差最大。固定减 180 会把满电的 2461 算成 2281，
        //    落到 60~80 区间，实测显示 68%（电池其实是满的）。
        //
        //    改为按端电压分档给压差：越高减得越少，饱满时直接不减。
        int drop;
        if      (raw_avg >= 2440) drop =   0;   // ≥4.19V，CV 末尾，压差可忽略
        else if (raw_avg >= 2380) drop =  40;   // 4.09~4.19V，接近满
        else if (raw_avg >= 2300) drop = 100;   // 3.95~4.09V，CC 中段偏上
        else if (raw_avg >= 2200) drop = 150;   // 3.78~3.95V，CC 中段
        else                      drop = 180;   // 更低，保持原来的粗估
        int est_raw = raw_avg - drop;
        if (est_raw < 0) est_raw = 0;
        show_pct = battery_raw_to_percent(est_raw);
        held     = true;
    }

    if (show_pct < 0)   show_pct = 0;
    if (show_pct > 100) show_pct = 100;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.raw          = raw_avg;
    s_state.mv           = mv_avg;
    s_state.percent      = show_pct;
    s_state.volt_pct     = volt_pct;
    s_state.charging     = charging;
    s_state.percent_held = held;
    s_state.valid        = true;
    xSemaphoreGive(s_lock);
}

static void bat_task(void *arg)
{
    (void)arg;
    while (1) {
        bat_sample_once();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        battery_state_t snap = s_state;
        xSemaphoreGive(s_lock);

        // 只在"显示电量变化"或"充放电切换"时打日志，避免每轮刷一行。
        // last_pct 初值 -999 保证第一轮必打 —— 否则开机去抖那几轮不打日志，
        // 等数值稳定下来又"没变化"，串口里就一行都看不到，排查时很误导。
        static int  last_pct  = -999;
        static bool last_chrg = false;
        if (snap.percent != last_pct || snap.charging != last_chrg) {
            last_pct  = snap.percent;
            last_chrg = snap.charging;
            ESP_LOGI(TAG, "电量 %d%%%s  (raw=%d, %dmV, 电压换算=%d%%, %s)",
                     snap.percent,
                     snap.percent_held ? "[保持]" : "",
                     snap.raw, snap.mv, snap.volt_pct,
                     snap.charging ? "充电中" : "放电");
        }

        /* 周期三选一：
         *   还没拿到过有效值        -> 2 秒   （攒够充电去抖，开机约 6 秒出数）
         *   充电口电平与稳定值不一致 -> 2 秒   （正在插拔，加速确认）
         *   其余                    -> 30 秒  （稳定运行）
         *
         * 为什么不能一律 30 秒：去抖要连续 CHRG_DEBOUNCE_N(3) 次同向采样，
         * 30 秒 × 3 意味着插上充电器后屏幕要 90 秒才变成"充电中"（原来
         * 2 秒周期时只要 6 秒）。这里每轮顺手读一次 GPIO（纯数字口，
         * 零成本），只要读到方向和当前稳定值不一致就立刻切回 2 秒，
         * 3 次采样后确认，再回 30 秒。插拔最坏延迟从 90 秒降到约 36 秒
         * （30 秒等待 + 6 秒确认）。 */
        bool chrg_now = (gpio_get_level(BOARD_CHRG_GPIO) == 1);
        bool switching = (chrg_now != snap.charging);
        uint32_t period = (!snap.valid || switching) ? BAT_WARMUP_PERIOD_MS
                                                     : BAT_SAMPLE_PERIOD_MS;
        vTaskDelay(pdMS_TO_TICKS(period));
    }
}

void battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);

    // ---- ADC：ADC2 / CH6 = GPIO17 ----
    // 用 memset + 逐字段赋值，而不是指定初始化器：
    // IDF 的结构体字段会随版本增删，-Werror=missing-field-initializers
    // 会把"少写一个字段"变成编译错误。memset 后零值恰好是各字段的合法默认
    // （clk_src=0 即 ADC_RTC_CLK_SRC_DEFAULT，ulp_mode=0 即 DISABLE）。
    adc_oneshot_unit_init_cfg_t u;
    memset(&u, 0, sizeof(u));
    u.unit_id = BAT_ADC_UNIT;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));

    adc_oneshot_chan_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.atten    = ADC_ATTEN_DB_12;
    cc.bitwidth = ADC_BITWIDTH_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BAT_ADC_CH, &cc));

    adc_cali_curve_fitting_config_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.unit_id  = BAT_ADC_UNIT;
    cal.chan     = BAT_ADC_CH;
    cal.atten    = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "ADC 校准不可用，百分比仍按 raw 换算");
    }

    // ---- 充电检测：GPIO38，输入，不加上下拉（与上游一致）----
    gpio_config_t chrg;
    memset(&chrg, 0, sizeof(chrg));
    chrg.pin_bit_mask = 1ULL << BOARD_CHRG_GPIO;
    chrg.mode         = GPIO_MODE_INPUT;
    chrg.pull_up_en   = GPIO_PULLUP_DISABLE;
    chrg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    chrg.intr_type    = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&chrg));

    /* ⚠ 检查 xTaskCreate 返回值。
     * 失败（内存不足）时任务不会跑，电量永远停在 "--"，
     * 而这里静默返回、开机日志一切正常 —— 这种"静默失效"最难查。 */
    if (xTaskCreate(bat_task, "bat", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "电量采样任务创建失败 —— 电量不会更新");
    }
    ESP_LOGI(TAG, "初始化完成: %s_CH%d (=GPIO%d), 充电检测 GPIO%d",
             "ADC2", BOARD_BAT_ADC_CH, BOARD_BAT_GPIO, BOARD_CHRG_GPIO);
}

void battery_get(battery_state_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}
