// main/apps/morse/morse_code.c —— ITU M.1677 电码表 + PARIS 计时编码。纯逻辑,不依赖 SDK。
#include "morse_code.h"

#include <stdbool.h>

typedef struct {
    char ch;
    const char *pattern;   // '.'=嘀 '-'=嗒,按发送顺序
} morse_entry_t;

static const morse_entry_t MORSE_TABLE[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
    {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
    {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
    {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
    {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
    {'Y', "-.--"},  {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
    {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"},
    {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
    {'$', "...-..-"}, {'@', ".--.-."},
};
#define MORSE_TABLE_COUNT (sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

// 把 ".-." 形式的字符串折成 bits/len。返回 false 表示串为空或超 7 元素。
static bool pattern_bits(const char *pattern, uint8_t *bits_out, uint8_t *len_out) {
    uint8_t bits = 0;
    uint8_t len = 0;
    for (const char *s = pattern; *s; s++) {
        if (len >= 7) return false;
        if (*s == '-') bits |= (uint8_t)(1u << len);
        len++;
    }
    if (len == 0) return false;
    *bits_out = bits;
    *len_out = len;
    return true;
}

morse_pattern_t morse_encode_char(char c) {
    morse_pattern_t result = {0, 0};
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (unsigned i = 0; i < MORSE_TABLE_COUNT; i++) {
        if (MORSE_TABLE[i].ch != c) continue;
        (void)pattern_bits(MORSE_TABLE[i].pattern, &result.bits, &result.len);
        return result;
    }
    return result;
}

char morse_decode_elements(uint8_t bits, uint8_t len) {
    if (len == 0 || len > 7) return '\0';
    for (unsigned i = 0; i < MORSE_TABLE_COUNT; i++) {
        uint8_t entry_bits = 0;
        uint8_t entry_len = 0;
        if (!pattern_bits(MORSE_TABLE[i].pattern, &entry_bits, &entry_len)) continue;
        if (entry_len == len && entry_bits == bits) return MORSE_TABLE[i].ch;
    }
    return '\0';
}

uint16_t morse_dit_ms(uint8_t wpm) {
    if (wpm == 0) wpm = 12;
    return (uint16_t)(1200u / wpm);
}

int morse_encode_text(const char *text, uint8_t wpm, morse_step_t *out, int max_steps) {
    if (!text) return 0;
    const uint16_t dit = morse_dit_ms(wpm);
    const uint16_t dah = (uint16_t)(3u * dit);
    const uint16_t char_gap = (uint16_t)(3u * dit);
    const uint16_t word_gap = (uint16_t)(7u * dit);

    int n = 0;
    int last_mark = -1;   // 上一个已登记步的下标,供词间隔回写
    for (int i = 0; text[i]; i++) {
        if (text[i] == ' ') {
            if (last_mark >= 0 && out && last_mark < max_steps) {
                out[last_mark].off_ms = word_gap;
            }
            continue;
        }
        morse_pattern_t pat = morse_encode_char(text[i]);
        if (pat.len == 0) continue;
        for (uint8_t e = 0; e < pat.len; e++) {
            const bool is_dah = (pat.bits & (uint8_t)(1u << e)) != 0;
            if (out && n < max_steps) {
                out[n].on_ms = is_dah ? dah : dit;
                out[n].off_ms = (e + 1 == pat.len) ? char_gap : dit;
                out[n].text_index = (uint8_t)i;
            }
            last_mark = n;
            n++;
        }
    }
    return n;
}
