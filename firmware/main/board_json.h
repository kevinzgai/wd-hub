/* 看板 JSON 解析
 *
 * 输出格式由我们自己的 bridge 完全控制，不需要通用解析器 ——
 * 一个键一个 strstr 就够，比 cJSON 省几十 KB Flash 和一次堆分配。
 * 局限（刻意接受）：不做完整 JSON 校验，只认"键:值"这种固定形状。
 */
#pragma once

#include "board_data.h"

/* 解析一份 /status 或 /screen/<i> 响应，整份覆盖 d。
 * 失败不会返回错误码 —— d->ok 由响应里的 "ok":true 决定，
 * 解析不出来的字段保留默认值（见 board_data.h 各字段注释）。 */
void board_parse(board_data_t *d, const char *json);
