// main/apps/morse/morse_tone.h —— 侧音发生器:约 700Hz 正弦门控输出,16kHz/16bit/单声道。
//
// 内部有独立音频流任务;bsp_audio_write 会阻塞,故本模块不碰 LVGL、不入按键回调。
// 音频所有权:begin 持 bsp_audio_init_playback(TX-only),end 在任务退出后
// bsp_audio_deinit 归还堆 —— 与其它前台音频应用同一约定。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 开始持续输出(初始为静音门控态)。幂等;失败返回错误且不残留任务。
esp_err_t morse_tone_begin(void);

// 开/关侧音。任意任务上下文可调,只置标志,非阻塞;
// 实际音头/音尾由音频任务做约 4ms 斜坡,避免爆音。
void morse_tone_gate(bool on);

// 停止输出、销毁音频任务并 bsp_audio_deinit()。幂等;
// 超时返回 ESP_ERR_TIMEOUT(调用方可重试,任务仍占着 codec)。
esp_err_t morse_tone_end(void);

#ifdef __cplusplus
}
#endif
