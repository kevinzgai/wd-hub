// main/apps/morse/morse_tone.c —— 侧音发生器实现:DDS 正弦查表 + 增益斜坡门控。
//
// 独立音频任务持续向 I2S 送 80 采样(5ms)小块,bsp_audio_write 的阻塞即为节拍。
// 门控只翻标志;音头/音尾由包络在约 4ms 内爬坡,避免方波式爆音。
#include "morse_tone.h"

#include "bsp_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE      16000
#define TONE_HZ            700
#define CHUNK_SAMPLES       80    // 5ms @16kHz:取消响应与 I2S 阻塞时长的折中
#define LUT_SIZE            64
#define AMPLITUDE        12000    // int16 峰值,留足余量防爆音失真
#define GAIN_MAX           256    // 定点增益 8.8
#define GAIN_STEP            4    // 256/4=64 采样 ≈ 4ms 斜坡
#define STOP_TIMEOUT_MS   2000

// 相位累加器步进:LUT 一周 = 2^22 相位单位,f = STEP * 16000 / 2^22 ≈ 700Hz。
#define PHASE_STEP ((uint32_t)(((uint64_t)TONE_HZ * LUT_SIZE * 65536u) / SAMPLE_RATE))

static int16_t s_lut[LUT_SIZE];
static bool s_lut_ready;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static volatile bool s_cancel;
static volatile bool s_gate;

static void lut_ensure(void) {
    if (s_lut_ready) return;
    const float pi2 = 6.283185307179586f;
    for (int i = 0; i < LUT_SIZE; i++) {
        s_lut[i] = (int16_t)(sinf(pi2 * (float)i / (float)LUT_SIZE) * AMPLITUDE);
    }
    s_lut_ready = true;
}

static void tone_task(void *arg) {
    (void)arg;
    int16_t buf[CHUNK_SAMPLES];
    uint32_t phase = 0;
    int gain = 0;
    while (!s_cancel) {
        const int target = s_gate ? GAIN_MAX : 0;
        for (int i = 0; i < CHUNK_SAMPLES; i++) {
            if (gain < target) gain += GAIN_STEP;
            else if (gain > target) gain -= GAIN_STEP;
            buf[i] = (int16_t)(((int)s_lut[(phase >> 16) & (LUT_SIZE - 1)] * gain) >> 8);
            phase += PHASE_STEP;
        }
        if (bsp_audio_write(buf, sizeof(buf)) != ESP_OK && !s_cancel) {
            // 写失败不做忙等;取消标志优先,stop 路径不受影响。
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    // 生命周期约定:先给信号量再挂起,句柄归 owner 删除。
    xSemaphoreGive(s_stopped);
    for (;;) vTaskSuspend(NULL);
}

esp_err_t morse_tone_begin(void) {
    if (s_task) return ESP_OK;
    lut_ensure();
    // TX-only:麦克风通道不分配,省电也省 DMA 堆。
    if (bsp_audio_init_playback() != ESP_OK) return ESP_FAIL;
    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        bsp_audio_deinit();
        return ESP_FAIL;
    }
    // 应用音量拉满,有效输出 = 系统主音量,侧音与系统声音同响度。
    bsp_audio_set_volume(100);

    if (s_stopped) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
    }
    s_stopped = xSemaphoreCreateBinary();
    if (!s_stopped) {
        bsp_audio_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_cancel = false;
    s_gate = false;
    if (xTaskCreate(tone_task, "morse_tone", 2048, NULL, 4, &s_task) != pdPASS) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
        bsp_audio_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void morse_tone_gate(bool on) {
    s_gate = on;
}

esp_err_t morse_tone_end(void) {
    TaskHandle_t task = s_task;
    if (!task) {
        if (s_stopped) {
            vSemaphoreDelete(s_stopped);
            s_stopped = NULL;
        }
        return ESP_OK;
    }
    s_gate = false;
    s_cancel = true;
    // 任务每 5ms 块检查一次取消标志,无需通知,等信号量即可。
    if (!s_stopped || xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;   // 任务仍在跑、codec 仍占用;调用方可重试
    }
    vTaskDelete(task);
    s_task = NULL;
    vSemaphoreDelete(s_stopped);
    s_stopped = NULL;
    bsp_audio_deinit();
    return ESP_OK;
}
