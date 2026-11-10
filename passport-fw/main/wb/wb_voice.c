// passport-fw/main/wb/wb_voice.c —— 录音会话：ES8311 采集 → IMA ADPCM →
// 环形缓冲 → 主机 poll 时按协议上行。
//
// 结构（三条上下文，互不阻塞）：
//   * 按键上下文（input task）：request_start / request_stop —— 只置原子标志；
//   * voice 任务：本文件私有任务。开 codec（wb_audio 换手 → duplex）→
//     20ms 一块读麦 → 编码入环 → 盯停止条件（松手 / 超时 / 断链 / 120s）→
//     收尾时还 codec 给 wb_audio；
//   * wb_task（BLE）：wb_voice_on_poll() 从环里取帧组一行 JSON 发出去。
//
// 内存红线（C3 无 PSRAM）：环 32 帧×163B + 行缓冲 2KB + 任务栈 4KB，
// 全部静态 BSS，运行期零分配。
//
// 与 wb_audio 的 codec 换手：录音期间 bsp_audio 归本模块（duplex，
// 顺带保证 RX 可读），wb_audio 的提示音自然拿不到 codec（INVALID_STATE
// → 本条跳过）；收尾 deinit 后 wb_audio_init() 恢复，提示音回归。
#include "wb_voice.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "bsp_audio.h"
#include "wb_adpcm.h"
#include "wb_audio.h"
#include "wb_link.h"
#include "wb_power.h"

static const char *TAG = "wb_voice";

// ---- 协议常量（与主机 wb_app.py 逐字对齐）----
#define VOICE_RATE          16000   // 主机 VOICE_SAMPLE_RATE
#define VOICE_MAX_SECONDS   120     // 主机 stop_reason=3 的硬上限
#define STOP_RELEASE        1       // 正常松开
#define STOP_NO_POLL        2       // 主机停止轮询
#define STOP_TIME_LIMIT     3       // 达到 120 秒上限
#define STOP_LINK_LOST      4       // 链路已断开

// ---- 采集/编码参数 ----
#define FRAME_MS            20
#define FRAME_SAMPLES       (VOICE_RATE * FRAME_MS / 1000)   // 320
// 一次 bsp_audio_read 的字节数（20ms 单声道 16bit）。
#define PCM_CHUNK_BYTES     (FRAME_SAMPLES * 2)

// ---- 环形缓冲 ----
// 32 帧 = 640ms：主机 100ms 一 poll、每 poll 取 5 帧正好实时；
// BLE 抖动 / poll 迟到时靠它垫，溢出丢最旧并计 dropped。
#define RING_SLOTS          32
#define BATCH_MAX           8      // 一行最多带的帧数（base64 后 ~1.9KB，wb_link 会按 MTU 切）
#define BATCH_NORMAL        5      // 100ms poll 对应 5 帧 = 实时

// 无 poll 视为主机不再消费：idle 档主机 1Hz 轮询，3s 足够宽容。
#define NO_POLL_TIMEOUT_US  (3000000ULL)

// ---- 状态 ----
typedef enum {
    VOICE_IDLE = 0,
    VOICE_RECORDING,     // 采集中
    VOICE_DRAINING,      // 已停采，环里还有尾巴没发完
} voice_state_t;

static struct {
    volatile voice_state_t state;
    volatile int stop_reason;

    uint32_t session;          // 会话号，递增
    uint32_t seq_next;         // 下一帧序号（编码侧）
    volatile uint32_t seq_sent; // 已发出的帧数（发送侧）

    // SPSC 环：生产者 voice 任务，消费者 wb_task（poll）。临界区保护。
    uint8_t ring[RING_SLOTS * WB_ADPCM_FRAME_BYTES];
    volatile unsigned head, tail, count;
    portMUX_TYPE ring_mux;

    // 统计（主机取各行的 max）
    volatile int peak;
    volatile unsigned dropped;
    volatile unsigned clipped;
    volatile uint32_t samples_total;

    int64_t start_us;
    volatile int64_t last_poll_us;

    // 任务管理
    TaskHandle_t worker;
    volatile bool stop_req;    // 按键请求松手
} s_vo = {
    .state = VOICE_IDLE,
    .ring_mux = portMUX_INITIALIZER_UNLOCKED,   // 零初始化不保证是解锁态
};

static wb_adpcm_state_t s_enc;   // 仅 voice 任务碰，无需锁

// 主机轮询帧间隔稳定后，每 poll 平均要搬 5 帧；这里给编码器一个 20ms 的
// 读缓冲。static 避免任务栈内大数组（C3 任务栈金贵）。
static int16_t s_pcm[FRAME_SAMPLES];
static uint8_t s_frame[WB_ADPCM_FRAME_BYTES];
static char s_line[2048];        // 一行 JSON（8 帧 base64 ≈ 1.9KB 封顶）
static uint8_t s_b64[(BATCH_MAX * WB_ADPCM_FRAME_BYTES + 2) / 3 * 4 + 4];

// ---- 环操作（生产者/消费者各一，临界区很短）----

static void ring_push(const uint8_t *frame)
{
    bool dropped = false;
    portENTER_CRITICAL(&s_vo.ring_mux);
    if (s_vo.count < RING_SLOTS) {
        memcpy(&s_vo.ring[s_vo.head * WB_ADPCM_FRAME_BYTES], frame, WB_ADPCM_FRAME_BYTES);
        s_vo.head = (s_vo.head + 1) % RING_SLOTS;
        s_vo.count++;
    } else {
        dropped = true;   // 满了丢最旧：新鲜度优先
        s_vo.tail = (s_vo.tail + 1) % RING_SLOTS;
        memcpy(&s_vo.ring[s_vo.head * WB_ADPCM_FRAME_BYTES], frame, WB_ADPCM_FRAME_BYTES);
        s_vo.head = (s_vo.head + 1) % RING_SLOTS;
    }
    portEXIT_CRITICAL(&s_vo.ring_mux);
    if (dropped) s_vo.dropped++;
}

// 取最多 max 帧到 out（连续布局），返回帧数。本批首帧序号经 *first_seq 带回。
static unsigned ring_pop(uint8_t *out, unsigned max, uint32_t *first_seq)
{
    unsigned n = 0;
    portENTER_CRITICAL(&s_vo.ring_mux);
    if (s_vo.count > 0) {
        *first_seq = s_vo.seq_sent;
        n = s_vo.count < max ? s_vo.count : max;
        for (unsigned i = 0; i < n; i++) {
            memcpy(out + i * WB_ADPCM_FRAME_BYTES,
                   &s_vo.ring[s_vo.tail * WB_ADPCM_FRAME_BYTES], WB_ADPCM_FRAME_BYTES);
            s_vo.tail = (s_vo.tail + 1) % RING_SLOTS;
        }
        s_vo.count -= n;
        s_vo.seq_sent += n;
    }
    portEXIT_CRITICAL(&s_vo.ring_mux);
    return n;
}

// ---- 上行 ----

static bool send_line(bool recording, uint32_t first_seq,
                      const uint8_t *frames, unsigned n, int stop_reason)
{
    size_t b64_len = 0;
    if (n > 0) {
        if (mbedtls_base64_encode((unsigned char *)s_b64, sizeof(s_b64), &b64_len,
                                  frames, n * WB_ADPCM_FRAME_BYTES) != 0) {
            return false;
        }
    }
    const uint32_t seconds =
        (uint32_t)((esp_timer_get_time() - s_vo.start_us) / 1000000ULL);
    (void)seconds;
    // peak/samples 主机按各行的 max 收，直接给累计值。
    int len = snprintf(s_line, sizeof(s_line),
                       "{\"voice\":1,\"session\":%lu,\"recording\":%d,"
                       "\"sequence\":%lu,\"pending\":%u,\"peak\":%d,"
                       "\"dropped\":%u,\"stop_reason\":%d,\"clipped\":%d,"
                       "\"samples\":%lu,\"adpcm\":\"%.*s\"}\n",
                       (unsigned long)s_vo.session, recording ? 1 : 0,
                       (unsigned long)first_seq, (unsigned)s_vo.count,
                       s_vo.peak, s_vo.dropped, stop_reason,
                       s_vo.clipped, (unsigned long)s_vo.samples_total,
                       (int)b64_len, (const char *)s_b64);
    if (len <= 0 || (size_t)len >= sizeof(s_line)) {
        ESP_LOGW(TAG, "语音行溢出（%d 帧）", n);
        return false;
    }
    return wb_link_send(s_line, (size_t)len);
}

void wb_voice_on_poll(void)
{
    if (s_vo.state == VOICE_IDLE) return;
    s_vo.last_poll_us = esp_timer_get_time();

    static uint8_t batch[BATCH_MAX * WB_ADPCM_FRAME_BYTES];
    uint32_t first_seq = 0;
    // 落后太多就多取几帧追赶，正常节奏 5 帧 = 实时。
    unsigned want = BATCH_NORMAL;
    portENTER_CRITICAL(&s_vo.ring_mux);
    if (s_vo.count > RING_SLOTS - 8) want = BATCH_MAX;
    portEXIT_CRITICAL(&s_vo.ring_mux);
    unsigned n = ring_pop(batch, want, &first_seq);

    const bool recording = (s_vo.state == VOICE_RECORDING);
    const int reason = recording ? 0 : s_vo.stop_reason;
    if (!recording && s_vo.count == 0) {
        // 尾巴排空 + 停录：最后一行带 recording=false，主机在这收会话。
        send_line(false, first_seq, batch, n, reason);
        s_vo.state = VOICE_IDLE;
        ESP_LOGI(TAG, "会话 %lu 结束 reason=%d 采样=%lu 丢帧=%u",
                 (unsigned long)s_vo.session, reason,
                 (unsigned long)s_vo.samples_total, s_vo.dropped);
        return;
    }
    send_line(recording, first_seq, batch, n, reason);
}

// ---- voice 任务 ----

static bool codec_take_for_capture(void)
{
    // wb_audio 正持有 playback profile 时必须先让位，否则 bsp_audio_init
    // 会因 profile 冲突返回 INVALID_STATE。
    wb_audio_deinit();
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "codec duplex 初始化失败");
        wb_audio_init();   // 至少把提示音通道还回去
        return false;
    }
    if (bsp_audio_set_format(VOICE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "16kHz 采集格式设置失败");
        bsp_audio_deinit();
        wb_audio_init();
        return false;
    }
    return true;
}

static void codec_give_back(void)
{
    bsp_audio_deinit();
    if (!wb_audio_init()) ESP_LOGW(TAG, "提示音通道恢复失败（重启前不可用）");
}

// 开录瞬间主动发一行：主机不用等下一次 poll（idle 档 1Hz）就能按下
// 听写热键，省掉最多 1s 的头部丢失。
static void announce_start(void)
{
    send_line(true, 0, NULL, 0, 0);
}

static void voice_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "会话 %lu 开始", (unsigned long)s_vo.session);

    if (!codec_take_for_capture()) {
        s_vo.state = VOICE_IDLE;
        s_vo.worker = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_vo.start_us = esp_timer_get_time();
    s_vo.last_poll_us = s_vo.start_us;
    s_vo.seq_next = 0;
    s_vo.seq_sent = 0;
    s_vo.peak = 0;
    s_vo.dropped = 0;
    s_vo.clipped = 0;
    s_vo.samples_total = 0;
    s_vo.stop_req = false;
    wb_adpcm_init(&s_enc);
    announce_start();

    while (true) {
        // 停止条件（采集阶段）：松手 / 无 poll / 断链 / 120s 上限。
        const int64_t now = esp_timer_get_time();
        if (s_vo.stop_req) { s_vo.stop_reason = STOP_RELEASE; break; }
        if (now - s_vo.last_poll_us > NO_POLL_TIMEOUT_US) { s_vo.stop_reason = STOP_NO_POLL; break; }
        if (!wb_link_connected()) { s_vo.stop_reason = STOP_LINK_LOST; break; }
        if (now - s_vo.start_us >= (int64_t)VOICE_MAX_SECONDS * 1000000LL) {
            s_vo.stop_reason = STOP_TIME_LIMIT;
            break;
        }

        if (bsp_audio_read(s_pcm, PCM_CHUNK_BYTES) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 峰值/削顶统计 + ADPCM 编码入环。
        int peak = s_vo.peak;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            const int v = s_pcm[i] < 0 ? -s_pcm[i] : s_pcm[i];
            if (v > peak) peak = v;
        }
        s_vo.peak = peak;
        if (peak >= 32700) s_vo.clipped++;
        s_vo.samples_total += FRAME_SAMPLES;
        wb_adpcm_encode_frame(&s_enc, s_pcm, s_frame);
        ring_push(s_frame);
        s_vo.seq_next++;

        // 采集期间保持活跃：待机没进来（每秒喂一次活动即可），BLE 保持快连档。
        if ((s_vo.samples_total / FRAME_SAMPLES) % 50 == 0)   // 每秒
            wb_power_note_activity();
    }

    // 停采 → DRAINING：环里的尾巴由 poll 驱动排空，最后一行在
    // wb_voice_on_poll() 里发。若主机从此不再 poll，状态停在这里，
    // 下一次 request_start 会强制复位。
    s_vo.state = VOICE_DRAINING;
    ESP_LOGI(TAG, "停采 reason=%d，待排空 %u 帧", s_vo.stop_reason, s_vo.count);

    codec_give_back();
    wb_audio_play(WB_SFX_KEY);   // 停录反馈（提示音通道已恢复）

    // DRAINING 状态本任务已无事可做；等 poll 排空后由 on_poll 置回 IDLE。
    // 但任务得退出释放栈 —— 用一个短驻留循环盯 IDLE，最多等 5s。
    for (int i = 0; i < 250 && s_vo.state != VOICE_IDLE; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (s_vo.state != VOICE_IDLE) {
        ESP_LOGW(TAG, "尾巴排空超时，强制复位会话");
        s_vo.state = VOICE_IDLE;
    }
    s_vo.worker = NULL;
    vTaskDelete(NULL);
}

// ---- 对外接口 ----

bool wb_voice_request_start(void)
{
    if (s_vo.state != VOICE_IDLE) return false;
    if (!wb_link_connected()) return false;
    // 上一次会话的尾巴没排干净（异常路径）就先复位，会话号照常递增。
    portENTER_CRITICAL(&s_vo.ring_mux);
    s_vo.head = s_vo.tail = s_vo.count = 0;
    s_vo.seq_sent = 0;
    portEXIT_CRITICAL(&s_vo.ring_mux);
    s_vo.session++;
    s_vo.state = VOICE_RECORDING;   // 先置位：关机手势抑制必须立刻生效
    s_vo.stop_reason = 0;
    if (xTaskCreate(voice_worker, "wb_voice", 4096, NULL, 5, &s_vo.worker) != pdPASS) {
        ESP_LOGE(TAG, "voice 任务创建失败");
        s_vo.state = VOICE_IDLE;
        return false;
    }
    ESP_LOGI(TAG, "请求开始录音 session=%lu", (unsigned long)s_vo.session);
    return true;
}

void wb_voice_request_stop(void)
{
    if (s_vo.state == VOICE_RECORDING) s_vo.stop_req = true;
}

bool wb_voice_is_active(void)
{
    return s_vo.state != VOICE_IDLE;
}
