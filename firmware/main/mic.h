// 麦克风采集（I2S RX）——语音上行的第一步。
//
// 硬件：这块板（xingzhi-cube-1.54tft 同款布局）的麦克风是一颗数字 MEMS
// （INMP441 一类），走独立的 I2S 总线，与喇叭完全分开：
//     MIC_SCK(GPIO5) / MIC_WS(GPIO4) / MIC_DIN(GPIO6)
// 喇叭占的是 GPIO15/16/7，正好是 ESP32-S3 的另一个 I2S 控制器，
// 所以录音用 I2S_NUM_1、播放用 I2S_NUM_0，两者可以同时存在互不干扰。
//
// 采到的数据格式：16kHz 单声道 int16。选 16k 是为了跟主机侧
// wb_app.py 的 VOICE_SAMPLE_RATE 对齐（那边按 16k 做 ADPCM 和 ASR）。
//
// 本文件目前只做采集，不做编码、不走 BLE —— 先用回环（录完直接播）
// 确认硬件通路、增益和噪声，再接 ADPCM 上行。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 I2S 麦克风。失败只打日志返回 false，不 abort ——
// 录不了音不应该让整个控制台起不来。
bool mic_init(void);

// 采集 ms 毫秒的单声道 16bit PCM 写进 out，最多 max_samples 个采样。
// 返回实际采到的采样数；0 表示不可用或读失败。
// 调用方负责提供缓冲（2 秒 16kHz 需要 64000 字节，建议放 PSRAM）。
size_t mic_record(int16_t *out, size_t max_samples, int ms);

// 丢弃当前 DMA 里积压的旧采样。
// 麦克风是连续采样的，I2S 一 enable 就一直在往 DMA 里填；如果不管它，
// 下次 mic_record 开头读到的会是几秒前的旧声音。录音前先 flush 一次。
void mic_flush(void);

// 短测：采集 ms 毫秒并返回这段时间的峰值（0..32767）。
// 用来在没有喇叭回环的情况下判断"麦克风到底有没有信号"。
int mic_probe_peak(int ms);

#ifdef __cplusplus
}
#endif
