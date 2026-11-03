#pragma once
#include "lvgl.h"
#include <string>
#include <string_view>

namespace passport {
// Display-only substitution; keep the original bytes for Wi-Fi authentication.
inline std::string ssid_display(std::string_view text, const lv_font_t *font) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = text[i];
        size_t n = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 0;
        bool valid = n && i + n <= text.size();
        uint32_t cp = n == 1 ? lead : lead & ((1u << (7 - n)) - 1);
        for (size_t k = 1; valid && k < n; ++k) {
            const unsigned char next = text[i + k];
            valid = (next & 0xc0) == 0x80; cp = (cp << 6) | (next & 63);
        }
        valid = valid && cp >= 32 && cp != 127 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) &&
                (n == 1 || (n == 2 && cp >= 0x80) || (n == 3 && cp >= 0x800) || (n == 4 && cp >= 0x10000));
        lv_font_glyph_dsc_t glyph{};
        if (valid && lv_font_get_glyph_dsc(font, &glyph, cp, 0)) out.append(text.substr(i, n));
        else out += '?';
        i += valid ? n : 1;
    }
    return out;
}
}
