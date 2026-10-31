// IMA ADPCM 编码器（4bit，每帧 320 采样 → 163 字节）。
//
// 与主机 wb_app.py 的 _adpcm_decode() **逐字节互逆**。改动这里必须同步跑
// tools/adpcm_crosscheck.py 做交叉验证，否则主机解出来的声音会变成噪声，
// 而且这种错误在设备端完全看不出来（编码器不会报错，只是数值对不上）。
//
// 帧格式（163 字节）：
//   [0..1]  int16 小端 predictor —— **编码前**的状态快照
//   [2]     int8  step index     —— 同样编码前的快照
//   [3..162] 160 字节 nibble，**低半字节在前**（一个字节里先出现的是前一个样本）
//
// 为什么每帧都带状态快照：解码器每帧从帧头重建状态，所以 BLE 丢一包
// 只会少 20ms 音频，不会让后面整段音频错乱（不需要额外的重同步逻辑）。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WB_ADPCM_SAMPLES      320     // 每帧采样数 = 20ms @16kHz
#define WB_ADPCM_FRAME_BYTES  163     // 3 字节头 + 160 字节 nibble

// 跨帧保持的编码器状态。predictor/index 就是上一帧结束时的值，
// 下一帧把它写进帧头，保证解码器能接着解。
typedef struct {
    int32_t predictor;
    int32_t index;      // 0..88
} wb_adpcm_state_t;

void wb_adpcm_init(wb_adpcm_state_t *st);

// 把 WB_ADPCM_SAMPLES 个 int16 编成一帧写入 out（需 WB_ADPCM_FRAME_BYTES 字节）。
// 状态在 st 里跨帧更新。
void wb_adpcm_encode_frame(wb_adpcm_state_t *st,
                           const int16_t *samples,
                           uint8_t *out);

#ifdef __cplusplus
}
#endif
