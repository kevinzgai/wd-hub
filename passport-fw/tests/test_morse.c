// tests/test_morse.c —— 摩斯电码表/编码序列/电键解码状态机的纯逻辑回归。
#include "apps/morse/morse_code.h"
#include "apps/morse/morse_decode.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_table_roundtrip(void) {
    // 表内每个字符:编码后再反查必须命中自己。
    const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?'!/()&:;=+-_\"$@";
    for (const char *p = chars; *p; ++p) {
        morse_pattern_t pat = morse_encode_char(*p);
        assert(pat.len >= 1 && pat.len <= 7);
        assert(morse_decode_elements(pat.bits, pat.len) == *p);
    }
    // 大小写不敏感;空格与表外字符不可编码。
    assert(morse_encode_char('s').len == 3);
    assert(morse_encode_char(' ').len == 0);
    assert(morse_encode_char('#').len == 0);
    assert(morse_decode_elements(0, 0) == '\0');
    assert(morse_decode_elements(0x7F, 8) == '\0');
}

static void test_dit_baseline(void) {
    assert(morse_dit_ms(12) == 100);   // PARIS:1200/12
    assert(morse_dit_ms(0) == 100);    // 兜底 12 WPM
    assert(morse_dit_ms(30) == 40);
    assert(morse_dit_ms(5) == 240);
}

static void test_encode_text(void) {
    morse_step_t steps[16];
    // "SOS" @12WPM:... --- ... → 9 步;嘀 on=100,嗒 on=300,
    // 码内 off=100,字符收尾 off=300。
    int n = morse_encode_text("SOS", 12, steps, 16);
    assert(n == 9);
    assert(steps[0].on_ms == 100 && steps[0].off_ms == 100 && steps[0].text_index == 0);
    assert(steps[2].on_ms == 100 && steps[2].off_ms == 300);   // S 收尾
    assert(steps[3].on_ms == 300 && steps[3].off_ms == 100);   // O 首嗒
    assert(steps[5].on_ms == 300 && steps[5].off_ms == 300);   // O 收尾
    assert(steps[8].text_index == 2);

    // 词间隔:空格把前一字符收尾扩成 7 嘀;连续空格不叠加。
    n = morse_encode_text("S  S", 12, steps, 16);
    assert(n == 6);
    assert(steps[2].off_ms == 700);
    assert(steps[5].off_ms == 300 && steps[5].text_index == 3);

    // 计数模式与截断:NULL 只计数;超出 max_steps 部分不写入。
    assert(morse_encode_text("SOS", 12, NULL, 0) == 9);
    memset(steps, 0xAA, sizeof(steps));
    assert(morse_encode_text("SOS", 12, steps, 2) == 9);
    assert(steps[2].on_ms == 0xAAAA);   // 未写入
}

static void test_decoder(void) {
    morse_decoder_t d;
    morse_decoder_init(&d, 12);   // dit=100ms

    // ".-" → 'A':先嘀(80ms<200)后嗒(300ms≥200),静默 300ms 出字符。
    morse_decoder_mark(&d, 80);
    assert(d.len == 1 && d.bits == 0);
    assert(morse_decoder_poll(&d, 100) == '\0');   // 码内静默不产出
    morse_decoder_mark(&d, 300);
    assert(d.len == 2 && d.bits == 2);
    assert(morse_decoder_poll(&d, 299) == '\0');
    assert(morse_decoder_poll(&d, 300) == 'A');
    assert(d.len == 0 && d.bits == 0);

    // 同一静默继续到 700ms 出词间隔;重复轮询不重复产出。
    assert(morse_decoder_poll(&d, 699) == '\0');
    assert(morse_decoder_poll(&d, 700) == ' ');
    assert(morse_decoder_poll(&d, 900) == '\0');

    // 未命中电码表的序列 → '?'(如 7 嘀);超 7 元素丢弃,直到一次产出清零。
    for (int i = 0; i < 7; ++i) morse_decoder_mark(&d, 50);
    for (int i = 0; i < 4; ++i) morse_decoder_mark(&d, 50);
    assert(d.len == 7);
    assert(morse_decoder_poll(&d, 300) == '?');
    assert(d.len == 0);

    // 空白静默不凭空出空格(全新解码器,从未有过按压)。
    morse_decoder_init(&d, 12);
    assert(morse_decoder_poll(&d, 5000) == '\0');

    // 调速丢弃半个字符并换基准。
    morse_decoder_mark(&d, 50);
    morse_decoder_set_wpm(&d, 30);   // dit=40ms
    assert(d.len == 0 && d.dit_ms == 40);
    morse_decoder_mark(&d, 100);     // ≥2*40 → 嗒
    assert(d.bits == 1 && d.len == 1);
    assert(morse_decoder_poll(&d, 120) == 'T');
}

int main(void) {
    test_table_roundtrip();
    test_dit_baseline();
    test_encode_text();
    test_decoder();
    puts("Morse: table roundtrip, PARIS timing, encode_text gaps, keyer decode PASS");
    return 0;
}
