/* 文本工具 —— 实现（从 main.cpp 原样搬出） */
#include "text_util.h"

#include <string.h>

#include "esp_log.h"

LV_FONT_DECLARE(font_cn_16);

static const char *TAG = "font";

void font_probe(void)
{
    static const struct { uint32_t cp; const char *name; } list[] = {
        { 0x20,   "空格" },
        { 0x5DE5, "工(中文)" },
        { 0x2605, "★" },
        { 0x26A1, "⚡" },
        { 0x2713, "✓" },
        { 0x2600, "☀" },
        { 0x25CF, "●" },
    };

    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        lv_font_glyph_dsc_t d;
        memset(&d, 0, sizeof(d));
        bool ok = lv_font_get_glyph_dsc(&font_cn_16, &d, list[i].cp, 0);

        /* ⚠ 只有"真的缺字"才告警。
         * 原来每次开机都打 7 条 WARN，正常字形也照打 —— 真正的告警
         * 混在里面，等于没有告警。现在：正常的降级成 DEBUG（默认不输出，
         * 需要排查时把日志级别调到 DEBUG 就能看到完整自检结果），
         * 缺字形（found=0 或渲染出来是占位方格）才升成 WARN。 */
        bool bad = (!ok) || d.is_placeholder;
        if (bad) {
            ESP_LOGW(TAG, "FONT %-9s U+%04X 缺失！found=%d placeholder=%d",
                     list[i].name, (unsigned)list[i].cp, (int)ok, (int)d.is_placeholder);
        } else {
            ESP_LOGD(TAG, "FONT %-9s U+%04X ok box=%dx%d ofs=(%d,%d) adv=%u",
                     list[i].name, (unsigned)list[i].cp,
                     (int)d.box_w, (int)d.box_h, (int)d.ofs_x, (int)d.ofs_y,
                     (unsigned)d.adv_w);
        }
    }
}

void utf8_sanitize(const lv_font_t *font, const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;

    while (*p && o + 1 < out_len) {
        uint32_t cp;
        int len;

        if (*p < 0x80)             { cp = *p;        len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { p++; continue; }

        bool valid = true;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }

        lv_font_glyph_dsc_t d;
        memset(&d, 0, sizeof(d));
        bool have = lv_font_get_glyph_dsc(font, &d, cp, 0)
                    && !d.is_placeholder && d.adv_w > 0;

        if (have && o + (size_t)len < out_len) {
            memcpy(out + o, p, len);
            o += len;
        } else if (o + 1 < out_len) {
            out[o++] = '?';
        }
        p += len;
    }
    out[o] = '\0';
}
