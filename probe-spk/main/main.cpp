// 喇叭引脚探测 —— 独立测试固件，不参与正式构建。
//
// 目的：确认 GPIO 7/15/16 是否真的连着功放，以及 BCLK/LRCK 有没有接反。
//
// 为什么用"功能验证"而不是量电平：
//   手头没有示波器，量不了 I2S 波形；也无法确认物理通断。
//   但只要能发出声音，就证明这组引脚确实是功放通路 —— 比查文档和看
//   丝印更直接。听不到就换下一组，用声音本身当探针。
//
// 探测策略（按可能性排序，每组响两声"叮"）：
//   A. 上游参考的标准接法：BCLK=15 LRCK=16 DOUT=7
//   B. A 的时钟对调：BCLK=16 LRCK=15 DOUT=7
//   C. 只用 7/15/16 但 DOUT 与 BCLK 对调
//   D. WS 极性反转（有些功放要求 WS 低电平为左声道）
//   E. 换成 I2S1（少数板子把功放挂在 I2S1 上）
//   每组都在串口打印编号，听到哪组响就记下编号。
//
// 注意：麦克风脚 4/5/6 全程不碰。它们被 codec 偏置在 VDD/2，
// 误配置会产生噪音甚至损坏，board_config.h 里有警告。

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "spkprobe";

// ---- 参数 ----
#define PROBE_RATE      24000       // 与上游板级配置一致
#define PROBE_MS        400         // 每声时长
#define PROBE_VOL       40          // 幅度百分比（0..100），别开满，防刺耳

// 正弦波：880Hz(A5) -> 1319Hz(E6) 线性扫频，两声上行，容易听出"有声音"
//
// 相位必须用积分形式，不能写成 sin(2*pi*f*t)：
//   瞬时频率 f(t) = f0 + (f1-f0)*t/T
//   相位 phi(t) = ∫2*pi*f(t)dt = 2*pi*f0*t + pi*(f1-f0)*t^2/T
// 写成 sin(2*pi*f(t)*t) 会在扫频时产生明显的频率畸变和相位跳变。
static void fill_tone(int16_t *buf, int frames, float f0, float f1)
{
    const float dur = (float)frames / PROBE_RATE;
    const int   fade = PROBE_RATE / 100;     // 10ms 淡入淡出，消除爆音

    for (int i = 0; i < frames; i++) {
        float t = (float)i / PROBE_RATE;
        float ph = 2.0f * (float)M_PI * f0 * t
                 + (float)M_PI * (f1 - f0) * t * t / dur;
        float v = sinf(ph);

        float env = 1.0f;
        if (i < fade)                env = (float)i / fade;
        else if (i > frames - fade)  env = (float)(frames - i) / fade;

        int16_t s = (int16_t)(v * env * (PROBE_VOL / 100.0f) * 32000);
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;      // 左右都写，规避声道接反导致完全无声
    }
}

typedef struct {
    char      name[48];
    int       port;      // 0 = I2S0, 1 = I2S1
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    bool      ws_inv;
    int       beeps;     // 本组响几声 —— 用声音次数编码组号，靠耳朵区分
} probe_cfg_t;

// 用"响几声"编码组号：A 响 1 声、B 响 2 声、C 响 3 声、D 响 4 声、E 响 5 声。
// 原来每组都响 2 声，结果分不清是哪组响的 —— 耳朵数不清"一样的两次"
// 属于哪一组。改成次数编码后，数一下就知道，不需要看串口。
static const probe_cfg_t k_probes[] = {
    { "A: BCLK=15 LRCK=16 DOUT=7 (上游标准)",   0, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_7,  false, 1 },
    { "B: BCLK=16 LRCK=15 DOUT=7 (时钟对调)",   0, GPIO_NUM_16, GPIO_NUM_15, GPIO_NUM_7,  false, 2 },
    { "C: BCLK=7  LRCK=16 DOUT=15 (SD/BCLK对调)", 0, GPIO_NUM_7, GPIO_NUM_16, GPIO_NUM_15, false, 3 },
    { "D: A + WS 极性反转",                    0, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_7,  true,  4 },
    { "E: 同 A 但用 I2S1",                     1, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_7,  false, 5 },
};
#define PROBE_NUM (sizeof(k_probes) / sizeof(k_probes[0]))

// 试一组配置：成功返回 true（能建立通道并写满数据）
static bool try_one(const probe_cfg_t *p)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(
        p->port == 0 ? I2S_NUM_0 : I2S_NUM_1, I2S_ROLE_MASTER);
    chan.auto_clear_after_cb = true;     // 没数据时输出 0，避免残留噪音

    i2s_chan_handle_t tx = NULL;
    esp_err_t err = i2s_new_channel(&chan, &tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  i2s_new_channel 失败: %s", esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t cfg = {};
    cfg.clk_cfg  = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(PROBE_RATE);
    // 宏名是 I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG，不是 I2S_STD_SLOT_DEFAULT_CONFIG
    // （后者在 v6.1 不存在，编译报 not declared）
    cfg.slot_cfg = (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = p->bclk;
    cfg.gpio_cfg.ws   = p->ws;
    cfg.gpio_cfg.dout = p->dout;
    cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;   // 绝不碰麦克风
    cfg.gpio_cfg.invert_flags.ws_inv = p->ws_inv;

    err = i2s_channel_init_std_mode(tx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  init_std_mode 失败: %s", esp_err_to_name(err));
        i2s_del_channel(tx);
        return false;
    }

    err = i2s_channel_enable(tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  enable 失败: %s", esp_err_to_name(err));
        i2s_del_channel(tx);
        return false;
    }

    // 按 beeps 数量响，用次数编码组号（见 k_probes 注释）
    const int frames = PROBE_RATE * PROBE_MS / 1000;
    int16_t *buf = (int16_t *)malloc(frames * 2 * sizeof(int16_t));
    if (!buf) {
        i2s_channel_disable(tx);
        i2s_del_channel(tx);
        return false;
    }
    memset(buf, 0, frames * 2 * sizeof(int16_t));
    fill_tone(buf, frames, 880.0f, 1319.0f);

    size_t written = 0;
    for (int rep = 0; rep < p->beeps; rep++) {
        esp_err_t w = i2s_channel_write(tx, buf, frames * 2 * sizeof(int16_t),
                                       &written, 1000);
        if (w != ESP_OK) {
            ESP_LOGW(TAG, "  write 失败: %s", esp_err_to_name(w));
            break;
        }
        ESP_LOGI(TAG, "  第 %d/%d 声", rep + 1, p->beeps);
        // 响声之间留 200ms 静音，否则连成一片数不出来
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(buf);
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    return true;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "===== 喇叭引脚探测开始 =====");
    ESP_LOGI(TAG, "共 %d 组，每组响的**次数**等于组号：", (int)PROBE_NUM);
    ESP_LOGI(TAG, "  A 响 1 声 / B 响 2 声 / C 响 3 声 / D 响 4 声 / E 响 5 声");
    ESP_LOGI(TAG, "请数一下响了几声，就知道是哪一组在响");
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (int i = 0; i < (int)PROBE_NUM; i++) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "---- 第 %d/%d 组: %s (应响 %d 声) ----",
                 i + 1, (int)PROBE_NUM, k_probes[i].name, k_probes[i].beeps);
        bool ok = try_one(&k_probes[i]);
        ESP_LOGI(TAG, "  结果: %s", ok ? "已发送" : "配置失败");

        // 组间停 3 秒，留出"数完上组"的时间
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "===== 探测结束 =====");
    ESP_LOGI(TAG, "如果一组都没听到，说明功放不在 7/15/16，");
    ESP_LOGI(TAG, "需要扩大扫描范围或检查硬件。");
    ESP_LOGI(TAG, "如果有某组响了，请告诉我组编号(1-%d)。", (int)PROBE_NUM);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "等待中... (探测已完成)");
    }
}
