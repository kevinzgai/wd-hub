// 提示音实现。设计说明见 audio.h 头部注释。//
// 本文件只做一件事：把预渲染好的 PCM 采样表 + 音量复制成立体声，写进 I2S。
// 没有任何振荡器、包络、泛音计算 —— 那些全部在离线端做完
// （tools/gen_tones_from_wav.py），固件里只剩 memcpy 级别的开销。
//
// 为什么不在运行时合成
// --------------------
// 之前这里是实时合成（和弦 + 非谐泛音 + 指数衰减 + tanh 软限幅），
// 技术上做对了，但用户一耳朵判定"还是电子音，不像乐器"。
// 量化后找到根因：合成器的**音色是恒定的** ——
//
//    泛音/基频能量比      20ms      260ms
//    合成音 v2           0.427      0.435     ← 几乎不变
//    真实乐器            0.190      0.005     ← 衰减 38 倍
//
// 真实乐器被敲响后频谱会从"亮"变"暗"，这个变化是"自然感"的核心。
// 用固定倍率叠加泛音，无论怎么调衰减时间，泛音都是按同一比例跟着基频
// 一起衰减的，所以音色永远不变 —— 这是合成器的定义性特征，不是参数问题，
// 因此不修了，直接换成真实采样。
//
// 五段提示音的采样来源见 tone_pcm.h（同一段铃声拆成单音后变调重组，
// 音色天然统一，靠音高/音数/节奏区分语义）。
#include "audio.h"

#include <string.h>
#include <stdlib.h>     // NULL

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "board_config.h"
#include "tone_pcm.h"     // 五段 PCM 采样表（由 tools/gen_tones_from_wav.py 生成）

static const char *TAG = "audio";

// ---- 参数 ----
#define AUDIO_RATE      24000     // 必须与上游板级配置一致（AUDIO_OUTPUT_SAMPLE_RATE）

/* ⚠ 采样率必须和 tone_pcm.h 里的 TONE_RATE 一致。
 *
 * 这两个数字分别住在两个文件里，原来只靠注释约定。改了一处 = 提示音
 * 变速变调（24k 的表按 16k 播出来，快 1.5 倍），而且不报任何错 ——
 * 听着就是"提示音变得很尖"，排查起来毫无线索。
 *
 * 用编译期断言把它钉死：谁改了采样率，另一个文件不改就编不过。
 *
 * ⚠ 必须写 static_assert，不能写 _Static_assert。
 * 本文件是 .cpp（C++ 编译），_Static_assert 是 C11 的关键字，
 * 在 C++ 里会被当成一个未声明的函数名，直接报
 *     error: expected constructor, destructor, or type conversion before '(' token
 * 而 static_assert 从 C++11 起就是关键字，不需要任何头文件。 */
static_assert(TONE_RATE == AUDIO_RATE,
              "TONE_RATE (tone_pcm.h) 必须等于 AUDIO_RATE (audio.cpp)，"
              "否则提示音会变速变调。改一个就要改另一个。");

// 默认音量百分比。用户实测 35% 偏大，降到 20%。
// 四段通知音的响度已在离线端按 RMS 拉齐（极差 6%），所以这里用统一比例就够，
// 不需要再按段补偿。按键音（KEY）在离线端又单独压了 6dB，那是刻意的设计，
// 不要在这里把它"补"回来 —— 原因见 audio.h 的枚举注释。
#define AUDIO_VOL_DEF   20
#define AUDIO_QUEUE_LEN 4         // 事件队列深度。4 足够，多出来的丢弃
#define AUDIO_TASK_PRIO 3         // 与按键任务同级，低于 fetch(5) 和 lvgl(4)
#define AUDIO_TASK_STACK 3072

static i2s_chan_handle_t s_tx;
static QueueHandle_t     s_q;
static volatile int      s_volume = AUDIO_VOL_DEF;
static bool              s_ready;

// 播放用的交错立体声缓冲。
//
// 在 init 时用 heap_caps_malloc 一次性分配并**永不释放**，而不是每次
// 播放都 malloc/free：
//   1) 服务端每 5 秒推一轮，意味着高频次分配，长期跑必留堆碎片；
//   2) 明确要求 MALLOC_CAP_INTERNAL —— 内部 RAM 才能被 I2S DMA 直接搬运。
//      裸 malloc 可能落到 PSRAM，DMA 从 PSRAM 取数据要靠 bounce buffer，
//      这里没必要冒这个风险。
//
// 长度按**最长的一段**算：ATTENTION 233ms 目前最长，留足余量到 1.2 秒，
// 以后加更长的采样子也不用改这里。1.2s × 24000 × 2ch × 2B = 115KB。
// 内部 RAM 够（8MB PSRAM 用不上，DMA 要求内部），实测分配成功。
#define AUDIO_MS_MAX     1200
#define AUDIO_FRAMES_MAX (AUDIO_RATE * AUDIO_MS_MAX / 1000)
static int16_t *s_buf;

// ===========================================================================
// 播放
// ===========================================================================

// 把一张单声道采样表播出去。
//
// 采样表是 24kHz 单声道，与 I2S 输出格式的差异只有"展开成交错立体声"：
// 把每个样本复制到左右两个声道。没有重采样、没有格式转换。
//
// 为什么不能直接 i2s_channel_write 送原始数据
// --------------------------------------------
// 因为 I2S 配的是立体声（I2S_SLOT_MODE_STEREO），DMA 按 L/R 成对取数。
// 如果直接把单声道数据扔进去，左声道的样本会占到右声道的位置，
// 结果左右交错成两个不同时刻的波形 —— 听起来是失真加噪声，而不是单声道。
//
// 音量在展开时乘（pcm × vol），不是先转 int16 再乘：
// 后者在低音量时会把小于 1 的值截断成 0，累积成明显的量化噪声。
static void play_pcm(const int16_t *pcm, int frames)
{
    if (!s_ready || !s_buf || !pcm || frames <= 0) return;
    if (frames > AUDIO_FRAMES_MAX) frames = AUDIO_FRAMES_MAX;   // 防御性截断

    const float vol = s_volume / 100.0f;

    // 音量 100% 时省掉乘法循环，直接就是复制 —— 这是唯一会走到的分支
    // 之外的常见情形，顺手省掉一次 12000 次的浮点乘。
    if (s_volume >= 100) {
        for (int i = 0; i < frames; i++) {
            s_buf[i * 2]     = pcm[i];   // 左
            s_buf[i * 2 + 1] = pcm[i];   // 右
        }
    } else {
        for (int i = 0; i < frames; i++) {
            int32_t v = (int32_t)((float)pcm[i] * vol);
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            int16_t s = (int16_t)v;
            s_buf[i * 2]     = s;
            s_buf[i * 2 + 1] = s;
        }
    }

    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, s_buf,
                                     (size_t)frames * 2 * sizeof(int16_t),
                                     &written, 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s 写入失败: %s", esp_err_to_name(err));
    }
}

static void audio_task(void *arg)
{
    audio_event_t ev;
    while (1) {
        if (xQueueReceive(s_q, &ev, portMAX_DELAY) != pdTRUE) continue;

        switch (ev) {
        case AUDIO_EV_DONE:
            // 叮—咚，下行两个音。下行 + 双音 = "完成、落地"，
            // 是四段里唯一的两音结构，所以和"提醒"绝不会混。
            ESP_LOGI(TAG, "提示音: 任务完成");
            play_pcm(k_tone_done, TONE_DONE_LEN);
            break;
        case AUDIO_EV_ATTENTION:
            // 同一音重复两次。重复制造"未完成感"，正是人催人时的本能节奏；
            // 刻意不用下行——下行是 DONE 的语义，会让人以为事情做完了。
            ESP_LOGI(TAG, "提示音: 等待操作");
            play_pcm(k_tone_attn, TONE_ATTN_LEN);
            break;
        case AUDIO_EV_CONNECTED:
            // 单声短高音，设备自报状态，不是给你消息。
            ESP_LOGI(TAG, "提示音: 已连接");
            play_pcm(k_tone_conn, TONE_CONN_LEN);
            break;
        case AUDIO_EV_SESSION_END:
            // 单声低音。收工是"告一段落"，轻一些、暗一些，不需要双音的圆满感。
            ESP_LOGI(TAG, "提示音: 会话收工");
            play_pcm(k_tone_end, TONE_END_LEN);
            break;
        case AUDIO_EV_KEY:
            // 按键反馈：110ms，音高 -2 半音（1247Hz），且离线端已单独压低 6dB。
            //
            // 不用 ESP_LOGI 记这一行 —— 它会被连按刷满串口，把真正有用的
            // 上行日志挤走。按键本来就有 button_action 的日志，这里不重复。
            play_pcm(k_tone_key, TONE_KEY_LEN);
            break;
        default:
            break;
        }
    }
}

// ===========================================================================
// 对外接口
// ===========================================================================

// 初始化中途失败时的统一回滚。四处错误分支的清理动作完全一样，
// 集中一处避免漏掉某个资源（尤其 s_buf 这 115KB）。
// 全程判断指针非空，可以在任意中间态调用。
static void audio_teardown(void)
{
    if (s_tx) {
        i2s_channel_disable(s_tx);   // 没 enable 过时返回错误，无害
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_ready = false;
}

void audio_init(void)
{
    // 播放缓冲放最前面申请：它占 115KB 内部 RAM，万一内存吃紧要第一时间
    // 暴露出来（下面会判空并关掉整个提示音功能），
    // 不能等到 I2S 通道都建好了才发现没内存。
    s_buf = (int16_t *)heap_caps_malloc(AUDIO_FRAMES_MAX * 2 * sizeof(int16_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGE(TAG, "播放缓冲分配失败（%d 字节），提示音不可用",
                 (int)(AUDIO_FRAMES_MAX * 2 * sizeof(int16_t)));
        return;
    }

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // 没数据时自动输出 0，避免 DMA 循环播放残留样本（听起来是嗡嗡声）
    chan.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s（提示音不可用）", esp_err_to_name(err));
        audio_teardown();
        return;
    }

    i2s_std_config_t cfg = {};
    cfg.clk_cfg  = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
    // 宏名是 I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG，v6.1 没有 I2S_STD_SLOT_DEFAULT_CONFIG
    cfg.slot_cfg = (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    // 引脚来自 board_config.h，已实测确认。
    // din 必须是 UNUSED —— 麦克风脚被偏置在 VDD/2，
    // 一旦被 I2S 当成输入采样会引入噪声，那三个脚也绝不能 gpio_config。
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = BOARD_SPK_BCLK;
    cfg.gpio_cfg.ws   = BOARD_SPK_LRCK;
    cfg.gpio_cfg.dout = BOARD_SPK_DOUT;
    cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

    err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init_std_mode 失败: %s", esp_err_to_name(err));
        audio_teardown();
        return;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel_enable 失败: %s", esp_err_to_name(err));
        audio_teardown();
        return;
    }

    s_q = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(audio_event_t));
    if (!s_q) {
        ESP_LOGE(TAG, "队列创建失败");
        audio_teardown();
        return;
    }

    if (xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        vQueueDelete(s_q);
        s_q = NULL;
        audio_teardown();
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "就绪: %dHz, 引脚 BCLK=%d LRCK=%d DOUT=%d, 音量 %d%%",
             AUDIO_RATE, BOARD_SPK_BCLK, BOARD_SPK_LRCK, BOARD_SPK_DOUT, s_volume);
}

void audio_play(audio_event_t ev)
{
    if (!s_ready || !s_q) return;
    if (ev == AUDIO_EV_NONE) return;
    // 队列满就丢，绝不在这里等 —— 调用方可能是 LVGL 任务
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "队列满，丢弃事件 %d", (int)ev);
    }
}

/* ⚠ 这里原来有个 audio_set_volume(int)，但全工程没有任何调用者。
 *
 * 按键（VOL_UP / VOL_DOWN）已经被背光占用了，提示音音量是固定的
 * AUDIO_VOL_DEF = 20%。留一个没人调的接口，容易让人以为"音量可调"，
 * 实际调不了 —— 比没有更误导。
 *
 * 真要开放音量，正确做法是加一个独立的配置入口（配置页 / NVS），
 * 再从那里调这个函数。届时把本函数加回来即可。 */
