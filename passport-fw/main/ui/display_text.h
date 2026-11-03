#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace passport {
// Server replies may contain emoji and full-width ASCII that the subset CJK
// fonts do not carry; LVGL would draw those as boxes. Drop pictographs and
// fold full-width forms to ASCII, keeping the CJK punctuation the font has.
inline bool display_keeps_fullwidth(uint32_t cp) {
    switch (cp) {
    case 0xFF01: case 0xFF08: case 0xFF09: case 0xFF0C:   // ！（），
    case 0xFF1A: case 0xFF1B: case 0xFF1F: case 0xFF5E:   // ：；？～
        return true;
    default:
        return false;
    }
}

inline bool display_drops(uint32_t cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||  // emoji, flags, skin tones
           (cp >= 0x2300 && cp <= 0x23FF) ||    // ⌚⏰ and other technical pictographs
           (cp >= 0x2600 && cp <= 0x27BF) ||    // misc symbols, dingbats
           (cp >= 0x2B00 && cp <= 0x2BFF) ||    // ⭐⬆ arrows/stars
           (cp >= 0xFE00 && cp <= 0xFE0F) ||    // variation selectors
           (cp >= 0xE0000 && cp <= 0xE007F) ||  // tag sequences
           cp == 0x200D || cp == 0x20E3;        // ZWJ, keycap
}

inline std::string fold_display_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 :
                              (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        bool valid = length != 0 && i + length <= text.size();
        uint32_t cp = length == 1 ? lead : length == 2 ? (lead & 0x1F) :
                      length == 3 ? (lead & 0x0F) : (lead & 0x07);
        for (size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            valid = (next & 0xC0) == 0x80;
            cp = (cp << 6) | (next & 0x3F);
        }
        if (!valid) {  // pass malformed bytes through untouched
            out.push_back(text[i++]);
            continue;
        }
        if (display_drops(cp)) {
            // skip pictographs
        } else if (cp == 0x3000) {
            out.push_back(' ');
        } else if (cp >= 0xFF01 && cp <= 0xFF5E && !display_keeps_fullwidth(cp)) {
            out.push_back(static_cast<char>(cp - 0xFEE0));
        } else {
            out.append(text.data() + i, length);
        }
        i += length;
    }
    return out;
}
}
