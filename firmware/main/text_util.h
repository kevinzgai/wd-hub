/* 文本工具：字体自检 + UTF-8 降级
 *
 * 两件事都只跟"字库认不认这个字"有关，和看板业务无关，所以从 main.cpp
 * 拆出来。渲染侧凡是要把服务端字符串直接上屏的地方，都先过 utf8_sanitize。
 */
#pragma once

#include <stddef.h>

#include "lvgl.h"

/* 开机自检几个关键字形（中文 / ★ / ⚡ / ✓ / ☀ / ●）。
 * 正常的降到 DEBUG（排查时调日志级别能看到完整结果），缺字才 WARN ——
 * 不然每次开机 7 条 WARN 会把真正的告警淹掉。 */
void font_probe(void);

/* 按字库逐字过滤 UTF-8：字库里没有的字降级成 '?'。
 *
 * 为什么需要：服务端下发的是任意字符串（文件名/项目名常带中文），而字库
 * 只烘焙了固定字符集。LVGL 遇到缺字会画占位方框，界面上很难看。
 * out 必须留得下 in 的长度 + 1（最坏情况每字符换成 '?'，长度不变短）。 */
void utf8_sanitize(const lv_font_t *font, const char *in, char *out, size_t out_len);
