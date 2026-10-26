// 提示音实现。设计说明见 audio.h 头部注释。
#include "audio.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>     // malloc / free

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "board_config.h"

static const char *TAG = "audio";

// ---- 参数 ----
#define AUDIO_RATE      24000     // 必须与上游板级配置一致（AUDIO_OUTPUT_SAMPLE_RATE）
// 默认音量百分比。用户实测 35% 偏大，降到 20%。
// 三种音效的响度感知不同（连接音最急促、完成音最长），
// 统一比例下连接音会显得更响，所以整体压到 20 更均衡。
#define AUDIO_VOL_DEF   20
#define AUDIO_QUEUE_LEN 4         // 事件队列深度。4 足够，多出来的丢弃
#define AUDIO_TASK_PRIO 3         // 与按键任务同级，低于 fetch(5) 和 lvgl(4)
#define AUDIO_TASK_STACK 3072

static i2s_chan_handle_t s_tx;
static QueueHandle_t     s_q;
static volatile int      s_volume = AUDIO_VOL_DEF;
static bool              s_ready;

// 播放用的交错立体声缓冲，AUDIO_RATE/2 帧（500ms）够放最长的 k_done(360ms)。
//
// 在 init 时用 heap_caps_malloc 一次性分配并**永不释放**，而不是每次
// 播放都 malloc/free 47KB：
//   1) 服务端每 5 秒推一轮，频响意味着高频次分配，长期跑必留堆碎片；
//   2) 明确要求 MALLOC_CAP_INTERNAL —— 内部 RAM 才能被 I2S DMA 直接搬运。
//      裸 malloc 可能落到 PSRAM，DMA 从 PSRAM 取数据要靠 bounce buffer，
//      这里没必要冒这个风险。
static int16_t *s_buf;
#define AUDIO_FRAMES_MAX (AUDIO_RATE / 2)

// ===========================================================================
// 音符与包络
// ===========================================================================

// 一个音：频率 + 时长。频率 0 表示静音间隔。
typedef struct {
    uint16_t freq;
    uint16_t ms;
} note_t;

// 完成：G5 -> C6 -> E6，上行大三度，听感"圆满、向上"
static const note_t k_done[] = {
    { 784, 90 }, { 0, 25 },
    { 1047, 90 }, { 0, 25 },
    { 1319, 130 },
};

// 等待：E6 -> A5，下行，听感"温和、提醒"
// 刻意比完成音更低更短，闭眼也能分清是哪种
static const note_t k_attention[] = {
    { 1319, 100 }, { 0, 30 },
    { 880, 150 },
};

// 连上服务端：A6 -> E7，两个短促高音急速上行，总长约 120ms。
//
// 三种音效必须互相可辨，所以从三个维度区分开：
//   完成    三音 上行 中音区(G5/E6) 总长 340ms  —— 圆满、有余韵
//   等待    两音 下行 高->中(E6/A5) 总长 280ms  —— 回落、提醒
//   连接    两音 上行 高音区(A6/E7) 总长 120ms  —— 最快最亮，像"嘀嘀"
// 连接音"短 + 高 + 上行"，即使和完成音连着响也不会混。
static const note_t k_connected[] = {
    { 1760, 55 }, { 0, 15 },
    { 2637, 60 },
};

// 会话收工（working 减少）：C6 -> E6，**两音**上行，中音区，总长约 220ms。
//
// 和"完成 todo"(k_done) 刻意区分：
//   k_done      三音 上行 G5->C6->E6  340ms  更"满"、有余韵
//   k_session   两音 上行 C6->E6      220ms  同调式但少一个音、更干脆
// 两者是同一条上行旋律的"完整版"和"简短版"，一听就能分出轻重，
// 但不会像完全不相关的两个音那样突兀 —— 它们表达的确实是同类事件
// （都是"事情办完了"），只是粒度不同。
static const note_t k_session_end[] = {
    { 1047, 100 }, { 0, 25 },
    { 1319, 110 },
};

// 把一串音符填进交错立体声缓冲。返回总帧数。
//
// 每个音独立做淡入淡出，而不是整段只做首尾：
// 音符之间如果不淡出，频率切换处会有明显的"咔"声。
static int synth(const note_t *notes, int n, int16_t *buf, int max_frames)
{
    int pos = 0;
    const int fade = AUDIO_RATE / 200;      // 5ms，短到听不出，够压掉咔声

    for (int k = 0; k < n; k++) {
        int frames = (int)((long)AUDIO_RATE * notes[k].ms / 1000);
        if (pos + frames > max_frames) frames = max_frames - pos;
        if (frames <= 0) break;

        float phase = 0.0f;
        float step  = 2.0f * (float)M_PI * notes[k].freq / AUDIO_RATE;

        // 音符短于 2*fade 时（<10ms）淡入淡出会互相吃掉，包络退化成
        // "只升不降"，末尾被硬切出咔声。当前最短音 55ms 碰不到，
        // 但改音符的人很容易踩到，这里把 fade 收紧到不超过半音长。
        int f = (frames / 2 < fade) ? frames / 2 : fade;
        if (f < 1) f = 1;

        for (int i = 0; i < frames; i++) {
            float v = (notes[k].freq == 0) ? 0.0f : sinf(phase);
            phase += step;

            // 单音包络：首尾各 f 帧线性斜坡
            float env = 1.0f;
            if (i < f)                  env = (float)i / f;
            else if (i > frames - f)    env = (float)(frames - i) / f;

            float amp = env * (s_volume / 100.0f) * 30000.0f;
            int16_t s = (int16_t)(v * amp);
            buf[(pos + i) * 2]     = s;   // 左
            buf[(pos + i) * 2 + 1] = s;   // 右
        }
        pos += frames;
    }
    return pos;
}

// ===========================================================================
// 播放
// ===========================================================================

static void play_notes(const note_t *notes, int n)
{
    if (!s_ready || !s_buf) return;

    // 单任务消费队列（只有 audio_task 调本函数），缓冲无需加锁复用
    int frames = synth(notes, n, s_buf, AUDIO_FRAMES_MAX);
    if (frames <= 0) return;

    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, s_buf, (size_t)frames * 2 * sizeof(int16_t),
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
            ESP_LOGI(TAG, "提示音: 任务完成");
            play_notes(k_done, sizeof(k_done) / sizeof(k_done[0]));
            break;
        case AUDIO_EV_ATTENTION:
            ESP_LOGI(TAG, "提示音: 等待操作");
            play_notes(k_attention, sizeof(k_attention) / sizeof(k_attention[0]));
            break;
        case AUDIO_EV_CONNECTED:
            ESP_LOGI(TAG, "提示音: 已连接");
            play_notes(k_connected, sizeof(k_connected) / sizeof(k_connected[0]));
            break;
        case AUDIO_EV_SESSION_END:
            ESP_LOGI(TAG, "提示音: 会话收工");
            play_notes(k_session_end, sizeof(k_session_end) / sizeof(k_session_end[0]));
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
// 集中一处避免漏掉某个资源（尤其 s_buf 这 47KB）。
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
    // 播放缓冲放最前面申请：它占 47KB 内部 RAM，万一内存吃紧要第一时间
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
    // din 必须是 UNUSED —— 麦克风脚被 codec 偏置在 VDD/2，
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

void audio_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    ESP_LOGI(TAG, "音量 -> %d%%", percent);
}
