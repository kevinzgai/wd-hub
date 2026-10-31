#include "mic.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"

static const char *TAG = "mic";

// 与主机 wb_app.py 的 VOICE_SAMPLE_RATE 一致。改这里必须同步改那边，
// 否则 ADPCM 帧的时长对不上，ASR 会把人声拉慢或赶快。
#define MIC_RATE        16000
// 一次 read 的帧数，以及 DMA 里挂几个这样的周期。
//
// 这两个数直接决定内部 RAM 占用：chunk × desc × 2声道 × 4字节。
// 最初设 240×6 = 11520 字节，结果把 lvgl 和 fetch 任务挤死
// （"任务创建失败 —— 屏幕不会刷新"）—— 这块板开蓝牙后内部 RAM 只剩十几 KB。
// 现在 160×3 = 3840 字节，省下 7.7KB。160 帧 = 10ms，对 100ms 一包的
// 语音上行仍然绰绰有余。
#define MIC_CHUNK       160
#define MIC_DMA_DESC    3
// INMP441 是 24bit 器件，I2S 按 32bit 槽读进来后取高 16 位就够用；
// 低 8 位是噪声，留在里面只会让后续乘法变慢。
#define MIC_SHIFT       16

static i2s_chan_handle_t s_rx;
static bool s_ready;

bool mic_init(void)
{
    if (s_ready) return true;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num  = MIC_DMA_DESC;
    chan.dma_frame_num = MIC_CHUNK;
    // auto_clear：主机侧读慢了/DMA 空了就送 0，而不是把上一次的旧数据
    // 循环读出来 —— 后者在波形上表现为一段被重复的噪声，很难查。
    chan.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s（录音不可用）", esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t cfg = {};
    cfg.clk_cfg  = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE);
    cfg.slot_cfg = (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = BOARD_MIC_SCK;
    cfg.gpio_cfg.ws   = BOARD_MIC_WS;
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;   // 麦克风只往里送数据，没有输出脚
    cfg.gpio_cfg.din  = BOARD_MIC_DIN;

    err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风 init_std_mode 失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return false;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风 enable 失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "麦克风就绪: %dHz 单声道, SCK=GPIO%d WS=GPIO%d DIN=GPIO%d",
             MIC_RATE, BOARD_MIC_SCK, BOARD_MIC_WS, BOARD_MIC_DIN);
    return true;
}

void mic_flush(void)
{
    if (!s_ready) return;
    // 读空当前 DMA 里的积压。每次最多读一个 DMA 周期（MIC_CHUNK 帧），
    // 读不到就说明已经空了 —— 用 0 超时，绝不在这里等。
    static int32_t scratch[MIC_CHUNK * 2];
    for (int i = 0; i < MIC_DMA_DESC + 2; i++) {
        size_t br = 0;
        if (i2s_channel_read(s_rx, scratch, sizeof(scratch), &br, 0) != ESP_OK) break;
        if (br == 0) break;
    }
}

size_t mic_record(int16_t *out, size_t max_samples, int ms)
{
    if (!s_ready || !out || max_samples == 0 || ms <= 0) return 0;

    // 临时缓冲必须比一次 read 大，且放内部 RAM（I2S DMA 只搬内部 RAM）。
    // 320 帧 = 20ms，是 16k 采样下比较整的一块。
    static int32_t tmp[MIC_CHUNK * 2];

    const size_t want = (size_t)((int64_t)MIC_RATE * ms / 1000);
    const size_t total = want < max_samples ? want : max_samples;

    size_t got = 0;
    while (got < total) {
        size_t frames = total - got;
        if (frames > MIC_CHUNK) frames = MIC_CHUNK;

        size_t br = 0;
        const esp_err_t err = i2s_channel_read(s_rx, tmp,
                                               frames * 2 * sizeof(int32_t), &br, 500);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "读麦克风失败: %s", esp_err_to_name(err));
            break;
        }
        const size_t n = br / (2 * sizeof(int32_t));   // 实际帧数
        if (n == 0) break;

        // 立体声交错：tmp[2i] 是左声道。INMP441 的 L/R 脚接地时只有左声道
        // 有数据（右声道全 0），所以固定取左。
        for (size_t i = 0; i < n; i++) {
            out[got + i] = (int16_t)(tmp[i * 2] >> MIC_SHIFT);
        }
        got += n;
    }
    return got;
}

int mic_probe_peak(int ms)
{
    if (!s_ready || ms <= 0) return 0;
    static int16_t buf[1600];   // 100ms @16k
    size_t n = mic_record(buf, sizeof(buf) / sizeof(buf[0]), ms);
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak) peak = v;
    }
    return peak;
}
