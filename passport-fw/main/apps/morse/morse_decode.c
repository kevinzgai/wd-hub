// main/apps/morse/morse_decode.c —— 电键解码状态机。纯逻辑,不依赖 SDK。
#include "morse_decode.h"
#include "morse_code.h"

void morse_decoder_init(morse_decoder_t *d, uint8_t wpm) {
    d->dit_ms = morse_dit_ms(wpm);
    d->bits = 0;
    d->len = 0;
    d->char_done = false;
    d->word_done = false;
}

void morse_decoder_set_wpm(morse_decoder_t *d, uint8_t wpm) {
    morse_decoder_init(d, wpm);
}

void morse_decoder_mark(morse_decoder_t *d, uint16_t press_ms) {
    if (d->len < 7) {
        if (press_ms >= (uint16_t)(2u * d->dit_ms)) {
            d->bits |= (uint8_t)(1u << d->len);
        }
        d->len++;
    }
    d->char_done = false;
    d->word_done = false;
}

char morse_decoder_poll(morse_decoder_t *d, uint16_t silent_ms) {
    // 先判字符间隔:一次长静默跨两档门限时,字符先出,词间隔留给下一次轮询。
    if (!d->char_done && d->len > 0 && silent_ms >= (uint16_t)(3u * d->dit_ms)) {
        d->char_done = true;
        const char c = morse_decode_elements(d->bits, d->len);
        d->bits = 0;
        d->len = 0;
        return c ? c : '?';
    }
    // 词间隔只跟在"本段静默真的产出过字符"之后,避免开机/空白段凭空出空格。
    if (d->char_done && !d->word_done && silent_ms >= (uint16_t)(7u * d->dit_ms)) {
        d->word_done = true;
        return ' ';
    }
    return '\0';
}
