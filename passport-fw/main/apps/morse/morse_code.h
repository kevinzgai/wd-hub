// main/apps/morse/morse_code.h —— 摩斯电码表与"文本→亮/灭序列"编码(纯逻辑,可 host 测试)。
//
// 只依赖 C 标准库,不碰 ESP-IDF/LVGL。计时基准为 PARIS:1 嘀 = 1200/WPM 毫秒,
// 嗒 = 3 嘀,码内间隔 = 1 嘀,字符间隔 = 3 嘀,单词间隔 = 7 嘀。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 一个字符的电码型:bit0 是第一个元素,0=嘀 1=嗒;len 为元素数(0=不可编码)。
typedef struct {
    uint8_t bits;
    uint8_t len;
} morse_pattern_t;

// 编码单个字符,大小写不敏感。空格与不支持字符返回 len=0(空格由序列层处理)。
morse_pattern_t morse_encode_char(char c);

// 由元素序列反查字符;未命中返回 '\0'。len 有效范围 1..7。
char morse_decode_elements(uint8_t bits, uint8_t len);

// PARIS 基准:WPM → 嘀时长(ms)。wpm=0 时按 12 WPM 兜底。
uint16_t morse_dit_ms(uint8_t wpm);

// 发报序列中的一步:亮 on_ms 毫秒,随后熄 off_ms 毫秒
// (off 已含码内/字符/单词间隔,播放方按顺序执行即可)。
// text_index 是该元素所属字符在原文中的下标,供 UI 高亮;文本长度须 < 256。
typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
    uint8_t text_index;
} morse_step_t;

// 把文本编成发报序列。返回总步数;out 为 NULL 时只计数。
// max_steps 之外的部分只计数不写入。不可编码字符跳过;空格把前一字符的
// 收尾间隔扩成 7 嘀词间隔(连续空格不叠加)。
int morse_encode_text(const char *text, uint8_t wpm, morse_step_t *out, int max_steps);

#ifdef __cplusplus
}
#endif
