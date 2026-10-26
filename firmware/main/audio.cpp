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
#include "tone_pcm.h"     // 完成音的 PCM 采样表（由 tools/gen_tone_from_mp3.py 生成）

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

// 播放用的交错立体声缓冲。
//
// 在 init 时用 heap_caps_malloc 一次性分配并**永不释放**，而不是每次
// 播放都 malloc/free：
//   1) 服务端每 5 秒推一轮，频响意味着高频次分配，长期跑必留堆碎片；
//   2) 明确要求 MALLOC_CAP_INTERNAL —— 内部 RAM 才能被 I2S DMA 直接搬运。
//      裸 malloc 可能落到 PSRAM，DMA 从 PSRAM 取数据要靠 bounce buffer，
//      这里没必要冒这个风险。
//
// 长度按**最长的一段**算：现在是完成音的 0.58 秒（TONE_PCM_LEN），
// 留一倍余量到 1.2 秒。合成出来的和弦音最长 0.45 秒，也在范围内。
// 1.2s × 24000 × 2ch × 2B = 115KB。内部 RAM 够（8MB PSRAM 没用，
// 但 DMA 要求内部），实测分配成功。
#define AUDIO_MS_MAX     1200
#define AUDIO_FRAMES_MAX (AUDIO_RATE * AUDIO_MS_MAX / 1000)
static int16_t *s_buf;

// ===========================================================================
// 音符与包络
// ===========================================================================

// 一个"音"最多由 3 个频率**同时**发声（也就是和弦），带自己的时长。
// 三个频率全 0 表示静音间隔。
//
// gain 是这一段的响度补偿档位（百分比）。放在每个 hit 上而不是每段音效上，
// 因为同一段里不同和弦的响度本来就不该一样 —— 例如 DONE 的最后一下
// 是"落地"，就该比前面那下重一点。
typedef struct {
    uint16_t f0, f1, f2;
    uint16_t ms;
    uint16_t gain;      // 响度补偿百分比，100 = 不补。用 16 位是因为
                        // CONNECTED 需要补到 340%，uint8_t 会溢出
} hit_t;

// ---- 音色：钟琴（tubular bell）的非谐泛音 ----
//
// 纯正弦听起来像电子表的"哔"，因为真实乐器没有一个只发单一频率的。
// 钟类乐器的泛音不是整数倍（那是有序的谐波），而是 2.76 / 5.40 这种
// 非谐比例 —— 正是这个"不准"让它有金属余韵，也让它一听就不像合成音。
// 马林巴/木琴同理（3.93 倍）。这里用钟琴的表，泛音少而亮，适合提示音。
#define HARM_MUL2   2.76f
#define HARM_MUL3   5.40f
#define HARM_AMP2   0.32f
#define HARM_AMP3   0.11f

// 泛音加到这个频率以上就直接跳过。
//
// 这不是保守估计，是必须的：采样率 24kHz 的 Nyquist 是 12kHz，
// C7(2093Hz) 的五次泛音算出来 11302Hz、G7(3136Hz) 更是 16934Hz，
// 一旦采进去会折返成完全无关的频率（aliasing），听起来是刺耳的沙沙声。
// 恰恰高音最需要泛音提亮，但高音的泛音最先越界 —— 所以这里逐音判断，
// 越界就不加，让高音自然退回纯正弦。低音区泛音充足、高音区干净，
// 这个副作用反而是我们想要的。
#define HARM_LIMIT  10800.0f

// 软限幅的拐点。29000 意味着：信号低于约 29000 时基本线性通过，
// 超过才平滑压弯，最大输出趋近 29000 而永远碰不到 32767 的硬边界。
#define SOFT_LIMIT  29000.0f

// ---- 响度补偿 ----
//
// 换用指数衰减包络后有个反直觉的结果：**峰值变高了，耳朵听着却更轻**。
// 实测（tools/verify_tone.py 仿真）：
//   原版 纯正弦+线性包络  峰值 6000   感知响度 2121
//   新版 和弦+指数衰减    峰值 11221  感知响度 1049
// 原因是能量全挤在起音后那 100ms，之后迅速衰减，耳朵做时间积分就觉得小。
// 峰值高只是"瞬间尖了一下"，不增加响度。
//
// 所以要按**感知响度**补，而不是按峰值补 —— 按峰值补会直接削顶爆音。
// 下面各段的 gain 是拿仿真把感知响度拉到 2000~2200 反推出来的，
// 改音符后要重跑 tools/verify_tone.py 复核，别凭感觉调。
//
//   段           感知响度(未补偿)   gain   补偿后
//   DONE              采样（真实铃声）    —     见 tone_pcm.h
//   SESSION_END       1167              180     2101
//   ATTENTION          838              250     2095
//   CONNECTED          631              340     2145
//
// 四段补偿后基本齐平（2095~2145），不会出现"连接提示最轻"这种问题。
// CONNECTED 补得最多，因为它只有两个单频、没有和弦叠加，
// 高音区泛音又全被 Nyquist 限制跳过，天生最单薄。
//
// ⚠️ DONE 已改为播放真实采样（tone_pcm.h），不再走合成路径。
// 原因是合成音怎么调泛音都还是"电子味" —— 实测用户一耳朵就听出来。
// 真实录制的铃声里有合成无法复现的东西：材质噪声、房间反射、
// 录制链路的频响。所以完成音用采样，另外三段仍用合成
// （它们只是状态提示，不需要"好听"，合成反而更可控）。

// ---- 余下三段提示音（完成音见 tone_pcm.h）----
//
// 设计原则：和弦比单音饱满得多，而"和弦进行"还能表达情绪。
// 全部用三和弦，且只用大三/小三两种，保证 2~3 个音同时响时不浑浊。

// 会话收工：**单个 Am 小三和弦**，240ms。
//
// 和 DONE 的区分靠调性而不是音数：DONE 是大调 + 两个和弦 + 解决感，
// 收工是小调 + 一个和弦 + 悬着没解决。这是"工作告一段落"而不是
// "任务圆满完成"，情绪上本来就该轻一些、暗一些。
// 小调色彩是最容易被耳朵分辨的差异，比"少一个音"清楚得多。
static const hit_t k_session_end[] = {
    { 880, 1047, 1319, 240, 180 },    // A5 + C6 + E6  = Am 小三和弦
};

// 等你操作：下行三度，E6+G6 -> A5+C6。
//
// 刻意**不用**完整三和弦：三和弦听起来像"结论"，而这里只是提醒。
// 两个音的三度音程（空五度、三度）留白多，像轻敲桌面，不刺耳。
// 从大三度滑到小三度同时下行，听觉上就是"往下落、在等你"。
static const hit_t k_attention[] = {
    { 1319, 1568,    0, 110, 220 },   // E6 + G6  大三度
    {    0,    0,    0,  35,   0 },
    {  880, 1047,    0, 190, 250 },   // A5 + C6  小三度
};

// 首次连上：C7 -> G7，纯正弦（高音区泛音会混叠，自动跳过）。
//
// 这一段**故意**保留电子感 —— 高音区本来就不该有泛音，
// 而"嘀嘀"这种清脆的电子音正好是"连接成功/开机"的通用听觉符号。
// 前三种是"给你听消息"，这一种是"设备在自报状态"，语气不同。
static const hit_t k_connected[] = {
    { 2093, 0, 0, 55, 340 },     // C7
    {    0, 0, 0, 15,   0 },
    { 3136, 0, 0, 60, 340 },     // G7
};

// 把一串和弦填进交错立体声缓冲。返回总帧数。
//
// 相比"每个音一根纯正弦"的版本，这里做了三件事，每件都直接决定好不好听：
//
// 1) 和弦：一个 hit 里 3 个频率同时发声。单个正弦是"哔"，三个音叠加
//    才有"和弦"的厚度。三个音的幅度要**各自除以 sqrt(3)**，
//    否则三个 0.9 的波叠起来会超过 1.0 直接削顶，听起来是破音。
//
// 2) 非谐泛音：每个音再叠 2.76 倍和 5.40 倍的弱泛音，这是钟琴的指纹。
//    越界（>HARM_LIMIT）的泛音跳过，避免高频混叠。
//
// 3) 指数衰减包络：这是"像乐器"还是"像电子音"的分水岭。
//    钟、木琴、钢琴被敲响后都是"瞬间到最高，然后指数衰落"，
//    而线性斜线（淡入淡出）听起来像门铃。所以用：
//      起音 3ms 线性上升（短到听不出，但能压掉咔声）
//      之后 e^(-4t/T) 指数衰减，T 是音符时长
//    指数衰减的尾巴很长，所以音符之间不需要硬切，自然就分开了。
static int synth(const hit_t *notes, int n, int16_t *buf, int max_frames)
{
    int pos = 0;
    // 起音 3ms。比原来的 5ms 更短，因为打击乐的"敲击感"就来自这个陡峭的起音。
    const int attack = AUDIO_RATE * 3 / 1000;

    // 三个频率同时响时的归一化系数。1/sqrt(3) ≈ 0.577
    const float chord_norm = 0.577f;

    for (int k = 0; k < n; k++) {
        int frames = (int)((long)AUDIO_RATE * notes[k].ms / 1000);
        if (pos + frames > max_frames) frames = max_frames - pos;
        if (frames <= 0) break;

        // 先把本 hit 参与发声的"基频 + 泛音"整理成一张小表，
        // 避免在逐样本的循环里反复判空和比较（那是 12000 次 × 3 频率）。
        float    osc_f[9];      // 频率
        float    osc_amp[9];    // 该分量的幅度（已含和弦归一化）
        int      osc_n = 0;

        const uint16_t base[3] = { notes[k].f0, notes[k].f1, notes[k].f2 };
        for (int j = 0; j < 3; j++) {
            if (!base[j]) continue;
            float f = (float)base[j];
            osc_f[osc_n] = f;             osc_amp[osc_n] = chord_norm;                osc_n++;
            if (f * HARM_MUL2 < HARM_LIMIT) {
                osc_f[osc_n] = f * HARM_MUL2; osc_amp[osc_n] = chord_norm * HARM_AMP2; osc_n++;
            }
            if (f * HARM_MUL3 < HARM_LIMIT) {
                osc_f[osc_n] = f * HARM_MUL3; osc_amp[osc_n] = chord_norm * HARM_AMP3; osc_n++;
            }
        }

        // ---- 振荡器初始化：用二阶递推代替逐样本 sinf ----
        //
        // 标准三角恒等式：sin((n+1)ω) = 2cos(ω)·sin(nω) − sin((n−1)ω)
        // 所以只要预存 2cos(ω) 和最初两个样本，之后每样本只需一次乘、一次减。
        //
        // 这不是为了省几个周期的微优化：最长一段 445ms 有 12000 个样本、
        // 最多 9 个振荡器，逐样本调 sinf 就是 10 万次超越函数，
        // 而 I2S DMA 缓冲区只有几十毫秒 —— 合成还没算完，声音已经断了。
        // 换成递推后同一段只需 2~3ms。
        //
        // 精度：float 在 12000 次递推内的累积误差约 1e-4，听感上不可闻。
        float osc_c[9];      // 2cos(ω)
        float prev[9];       // sin((n−1)ω)
        float cur[9];        // sin(nω)
        for (int j = 0; j < osc_n; j++) {
            float w = 2.0f * (float)M_PI * osc_f[j] / AUDIO_RATE;
            osc_c[j] = 2.0f * cosf(w);
            prev[j]  = -sinf(w);   // sin(−ω)，配合 cur=sin(0)=0 让首个输出为 0
            cur[j]   = 0.0f;
        }

        // ---- 包络：指数衰减也用递推 ----
        // e^(−decay·i) = (e^(−decay))^i，所以只用算一次 expf，
        // 之后每样本乘一个常数。衰减到 T 时刻约剩 e^-4 ≈ 1.8%，听感正好消失。
        const float decay    = 4.0f / (float)frames;
        const float env_step = expf(-decay);
        float env = 1.0f;
        float vol = s_volume / 100.0f;
        // 增益只作用在"要不要更响"，削顶保护靠下面的软限幅，
        // 不靠预先压低基数 —— 否则为了最响的一段，其他段会被一起拖轻。
        const float amp_base = vol * 30000.0f * (notes[k].gain / 100.0f);

        for (int i = 0; i < frames; i++) {
            float v = 0.0f;
            for (int j = 0; j < osc_n; j++) {
                v += cur[j] * osc_amp[j];
                float next = osc_c[j] * cur[j] - prev[j];
                prev[j] = cur[j];
                cur[j]  = next;
            }

            // 3ms 陡起音叠在指数衰减上：打击乐的"敲击感"就来自这个陡峭起音
            float e = env;
            if (i < attack) e *= (float)i / (float)attack;
            env *= env_step;

            float samp = v * e * amp_base;

            // ---- 软限幅 ----
            // 和弦峰值有随机性（三个频率的相位关系时刻在变），
            // 响度补偿又推高了整体电平，硬截到 int16 边界会爆音。
            // tanh 在 |x| 小的时候近似线性（几乎不改变音色），
            // 只在接近上限时平滑压弯，听感是"轻微压缩"而不是"破音"。
            samp = SOFT_LIMIT * tanhf(samp / SOFT_LIMIT);

            int16_t s = (int16_t)samp;
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

static void play_notes(const hit_t *notes, int n)
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

// 播放 PCM 采样表（完成音用的"叮咚"铃声）。
//
// 采样表是单声道 24kHz，与 I2S 输出格式的差异只有"交错立体声"这一步：
// 把每个样本复制到左右两个声道。没有重采样、没有格式转换，
// 所以这一步几乎不耗 CPU。
//
// 为什么不用 i2s_channel_write 直接送原始数据
// ----------------------------------------------
// 因为 I2S 配的是立体声（I2S_SLOT_MODE_STEREO），DMA 按 L/R 成对取数。
// 如果直接把单声道数据扔进去，左声道会播到右声道的位置，
// 结果就是左右交错成两个不同时刻的样本 —— 听起来是失真加噪声。
void play_pcm(const int16_t *pcm, int frames)
{
    if (!s_ready || !s_buf || !pcm || frames <= 0) return;
    if (frames > AUDIO_FRAMES_MAX) frames = AUDIO_FRAMES_MAX;   // 防御性截断

    float vol = s_volume / 100.0f;
    for (int i = 0; i < frames; i++) {
        // 乘音量后再转 int16。不能先转再乘 —— 那样会在低音量时累积截断误差。
        int32_t v = (int32_t)((float)pcm[i] * vol);
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        int16_t s = (int16_t)v;
        s_buf[i * 2]     = s;   // 左
        s_buf[i * 2 + 1] = s;   // 右
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
            // 完成音用真实采样（用户指定的"叮咚"铃声），不是合成音。
            // 合成怎么调都还是电子味，而真实录制的铃声一耳朵就是"铃声"。
            ESP_LOGI(TAG, "提示音: 任务完成");
            play_pcm(k_tone_pcm, TONE_PCM_LEN);
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
