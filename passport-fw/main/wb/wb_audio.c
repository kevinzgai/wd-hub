// passport-fw/main/wb/wb_audio.c —— WB 提示音的异步播放实现。
//
// 行为对齐 S3 看板固件（firmware/main/audio.cpp），但代码在本工程独立实现：
//   * 素材：与 S3 同一段 Gentle Marimba，已离线重采样成 16kHz（见 wb_tone_pcm.h）；
//   * 通道：ES8311 + 单喇叭，16kHz / 16bit / 单声道入参；
//   * 缓冲：400ms 立体声展开缓冲（25.6KB，内部 RAM），不碰 PSRAM；
//   * 队列：深度 4，满了就丢，绝不反压 LVGL / BLE；
//   * 音量：固定 20%（与 S3 相同），主机 cfg.beep_vol 只当静音开关用。
#include "wb_audio.h"

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdint.h>
#include <string.h>

#include "wb_tone_pcm.h"

static const char *TAG = "wb_audio";

#define WB_AUDIO_VOLUME      20     // 与 S3 AUDIO_VOL_DEF 一致
#define WB_AUDIO_CHUNK_MS    400    // 内部 RAM 红线下的上限（C3 最大连续块约 114KB）
#define WB_AUDIO_FRAMES_MAX  (WB_TONE_RATE * WB_AUDIO_CHUNK_MS / 1000)
#define WB_AUDIO_BUF_BYTES   (WB_AUDIO_FRAMES_MAX * 2 * sizeof(int16_t))  // 25,600B
#define WB_AUDIO_QUEUE_LEN   4
#define WB_AUDIO_TASK_STACK  3072
#define WB_AUDIO_TASK_PRIO   3      // 与 S3 同级：低于 LVGL(4) 和 BLE 收帧任务

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;
static int16_t *s_buf;
static volatile bool s_ready;
static volatile bool s_stop;
static volatile bool s_standby;
static volatile bool s_host_mute;
static bool s_codec_open;
static bool s_low_active;
static bool s_low_pending;

static volatile unsigned s_requests, s_played, s_dropped, s_muted, s_failures;

// ---------------------------------------------------------------------------
// codec
// ---------------------------------------------------------------------------
static bool codec_ensure(void)
{
    if (bsp_audio_get_master_volume() == 0) {
        s_muted++;
        return false;
    }
    if (s_codec_open) return true;

    esp_err_t err = bsp_audio_init_playback();
    if (err != ESP_OK) {
        // ESP_ERR_INVALID_STATE = 其他应用/开机音还占着 codec，下一条事件再试。
        s_failures++;
        ESP_LOGW(TAG, "codec 打开失败(%s)，本条提示音跳过", esp_err_to_name(err));
        return false;
    }
    if (bsp_audio_set_format(WB_TONE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "16kHz 格式设置失败");
        bsp_audio_deinit();
        s_failures++;
        return false;
    }
    bsp_audio_set_volume(WB_AUDIO_VOLUME);
    s_codec_open = true;
    ESP_LOGI(TAG, "codec 就绪: %dHz/16bit/单声道, 固定音量 %d%%",
             WB_TONE_RATE, WB_AUDIO_VOLUME);
    return true;
}

// 把一段单声道 PCM 展开成 L/R 交错立体声后分块写 I2S。
// 单喇叭也不必区分声道：两路都送，硬件接哪一路都能听到。
static bool play_one(wb_sfx_t sfx)
{
    const int16_t *pcm = NULL;
    size_t frames = 0;
    const char *name = "";
    switch (sfx) {
    case WB_SFX_ATTENTION: pcm = k_wb_tone_attn; frames = WB_TONE_ATTN_LEN; name = "ATTENTION"; break;
    case WB_SFX_DONE:      pcm = k_wb_tone_done; frames = WB_TONE_DONE_LEN; name = "DONE"; break;
    case WB_SFX_CONNECTED: pcm = k_wb_tone_conn; frames = WB_TONE_CONN_LEN; name = "CONNECTED"; break;
    case WB_SFX_END:       pcm = k_wb_tone_end;  frames = WB_TONE_END_LEN;  name = "SESSION_END"; break;
    case WB_SFX_KEY:       pcm = k_wb_tone_key;  frames = WB_TONE_KEY_LEN;  name = "KEY"; break;
    default: return false;
    }
    if (!pcm || !frames) return false;

    for (size_t offset = 0; offset < frames; offset += WB_AUDIO_FRAMES_MAX) {
        if (s_stop) return false;
        size_t n = frames - offset;
        if (n > WB_AUDIO_FRAMES_MAX) n = WB_AUDIO_FRAMES_MAX;
        for (size_t i = 0; i < n; ++i) {
            int32_t v = (int32_t)((float)pcm[offset + i] * (WB_AUDIO_VOLUME / 100.0f));
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            s_buf[i * 2] = (int16_t)v;
            s_buf[i * 2 + 1] = (int16_t)v;
        }
        if (bsp_audio_write(s_buf, n * 2 * sizeof(int16_t)) != ESP_OK) {
            s_failures++;
            ESP_LOGW(TAG, "%s 写入失败（%u 帧后中止）", name, (unsigned)offset);
            return false;
        }
    }

    // 10ms 静音把 DMA 里最后一块推完，避免截断成“咔”声。
    const size_t tail = WB_TONE_RATE / 100;
    memset(s_buf, 0, tail * 2 * sizeof(int16_t));
    (void)bsp_audio_write(s_buf, tail * 2 * sizeof(int16_t));

    s_played++;
    ESP_LOGI(TAG, "播放 %s: %u 帧(%.0fms), 累计 played=%u",
             name, (unsigned)frames, frames * 1000.0 / WB_TONE_RATE, s_played);
    return true;
}

static void audio_task(void *arg)
{
    (void)arg;
    uint8_t ev;
    while (true) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (s_stop || ev == WB_SFX_NONE) break;
        if (s_standby) {
            s_dropped++;
            ESP_LOGI(TAG, "待机中，丢弃提示音 %d", (int)ev);
            continue;      // 待机中不播放（包括被唤醒前到达的帧）
        }
        if (s_host_mute && ev != WB_SFX_KEY) {
            s_muted++;
            continue;
        }
        if (!codec_ensure()) continue;
        (void)play_one((wb_sfx_t)ev);
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
bool wb_audio_init(void)
{
    if (s_ready) return true;

    s_buf = (int16_t *)heap_caps_malloc(WB_AUDIO_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGE(TAG, "播放缓冲分配失败: %u 字节（内部最大块 %u）",
                 (unsigned)WB_AUDIO_BUF_BYTES,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    s_queue = xQueueCreate(WB_AUDIO_QUEUE_LEN, sizeof(uint8_t));
    s_done = xSemaphoreCreateBinary();
    if (!s_queue || !s_done) {
        ESP_LOGE(TAG, "队列/完成信号量创建失败");
        if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
        if (s_done) { vSemaphoreDelete(s_done); s_done = NULL; }
        heap_caps_free(s_buf);
        s_buf = NULL;
        return false;
    }

    s_stop = false;
    s_standby = false;
    s_host_mute = false;
    s_codec_open = false;
    s_ready = true;
    if (xTaskCreate(audio_task, "wb_audio", WB_AUDIO_TASK_STACK, NULL,
                    WB_AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        s_ready = false;
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_done);
        s_queue = NULL;
        s_done = NULL;
        heap_caps_free(s_buf);
        s_buf = NULL;
        return false;
    }

    ESP_LOGI(TAG, "提示音就绪: 缓冲 %uB(%dms), 队列 %d, 内部最大块 %u",
             (unsigned)WB_AUDIO_BUF_BYTES, WB_AUDIO_CHUNK_MS, WB_AUDIO_QUEUE_LEN,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

void wb_audio_deinit(void)
{
    if (!s_ready) return;
    s_ready = false;
    s_stop = true;
    uint8_t none = WB_SFX_NONE;
    xQueueReset(s_queue);
    (void)xQueueSend(s_queue, &none, 0);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1200)) != pdTRUE) {
        ESP_LOGW(TAG, "播放任务未在 1.2s 内退出，保留 codec 不释放");
        return;
    }
    if (s_codec_open) {
        bsp_audio_deinit();
        s_codec_open = false;
    }
    vQueueDelete(s_queue);
    vSemaphoreDelete(s_done);
    heap_caps_free(s_buf);
    s_queue = NULL;
    s_done = NULL;
    s_buf = NULL;
    s_task = NULL;
    ESP_LOGI(TAG, "提示音已释放: requests=%u played=%u dropped=%u muted=%u failures=%u",
             s_requests, s_played, s_dropped, s_muted, s_failures);
}

void wb_audio_play(wb_sfx_t sfx)
{
    if (!s_ready || sfx == WB_SFX_NONE) return;
    s_requests++;
    uint8_t ev = (uint8_t)sfx;
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        s_dropped++;
        ESP_LOGW(TAG, "队列满，丢弃提示音 %d", (int)sfx);
    }
}

void wb_audio_set_standby(bool standby)
{
    s_standby = standby;
    if (!standby && s_low_pending) {
        s_low_pending = false;
        ESP_LOGI(TAG, "唤醒后补报低电量提示");
        wb_audio_play(WB_SFX_ATTENTION);
    }
}

void wb_audio_set_host_mute(bool mute)
{
    s_host_mute = mute;
}

void wb_audio_low_battery(int soc)
{
    if (soc < 0) return;
    if (soc > 18) {          // 回差：充到 19% 才允许再次提醒
        s_low_active = false;
        s_low_pending = false;
        return;
    }
    if (soc <= 15 && !s_low_active) {
        s_low_active = true;
        if (s_standby) {
            s_low_pending = true;
            ESP_LOGI(TAG, "电量 %d%%：低电提示挂起（待机中不播放）", soc);
        } else {
            ESP_LOGI(TAG, "电量 %d%%：播放低电提示", soc);
            wb_audio_play(WB_SFX_ATTENTION);
        }
    }
}

void wb_audio_log_stats(void)
{
    ESP_LOGI(TAG, "状态: ready=%d codec=%d standby=%d host_mute=%d "
             "requests=%u played=%u dropped=%u muted=%u failures=%u",
             (int)s_ready, (int)s_codec_open, (int)s_standby, (int)s_host_mute,
             s_requests, s_played, s_dropped, s_muted, s_failures);
}
