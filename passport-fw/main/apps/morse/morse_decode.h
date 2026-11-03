// main/apps/morse/morse_decode.h —— 电键练习解码器:按压/静默时长 → 元素 → 字符(纯逻辑)。
//
// 用法:抬键时调 morse_decoder_mark(按压毫秒);静默期间周期调
// morse_decoder_poll(自上次抬键起的静默毫秒),跨字符间隔返回解码字符,
// 跨词间隔返回 ' '。元素未命中电码表时返回 '?' 让练习者看到错误。
// 所有调用必须在同一任务上下文串行进行(本结构无内部锁;调用方持外部互斥)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t dit_ms;     // 当前嘀基准(由 WPM 决定)
    uint8_t bits;        // 当前字符已累计的元素,bit0 为第一个,0=嘀 1=嗒
    uint8_t len;         // 已累计元素数(≤7)
    bool char_done;      // 本段静默已产出字符(防重复产出)
    bool word_done;      // 本段静默已产出词间隔
} morse_decoder_t;

void morse_decoder_init(morse_decoder_t *d, uint8_t wpm);

// 调整速度基准。会丢弃已累计的半个字符(新旧基准混用无意义)。
void morse_decoder_set_wpm(morse_decoder_t *d, uint8_t wpm);

// 抬键时调用:按压 < 2 嘀判嘀,否则判嗒;并重置本段静默的产出标志。
// 超过 7 个元素后继续按的元素被丢弃,直到一次字符产出清零。
void morse_decoder_mark(morse_decoder_t *d, uint16_t press_ms);

// 静默期间周期调用(建议 ≤30ms 一次)。silent_ms 为自上次抬键起的静默时长。
// 返回:解码出的字符 / ' '(词间隔) / '\0'(尚无产出)。
char morse_decoder_poll(morse_decoder_t *d, uint16_t silent_ms);

#ifdef __cplusplus
}
#endif
