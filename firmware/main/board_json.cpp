/* 看板 JSON 解析 —— 实现
 *
 * 全部从 main.cpp 原样搬来（parse_into → board_parse），逻辑一字未改。
 * 里面几条注释是踩过的坑，别在重构时删掉：
 *   · json_nth_object 必须区分字符串内外，否则标题里的 { } ] 会撕坏结构；
 *   · rows / sessions 的子串缓冲给 512，服务端一加字段不至于静默截断。
 */
#include "board_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

static bool json_str(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            out[i++] = (*p == 'n' || *p == 't') ? ' ' : *p;
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return true;
}

static int json_int(const char *json, const char *key, int fallback)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return fallback;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p == '"') return fallback;
    return atoi(p);
}

/* 定位 sessions 数组的第 n 个对象，返回起止指针 */
static bool json_nth_object(const char *json, const char *array_key, int n,
                            const char **start, const char **end)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":[", array_key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);

    int depth = 0, idx = -1;
    const char *obj_start = NULL;
    /* ⚠ 必须区分字符串内外。
     * 会话标题是 AI 生成的文本，里面出现 { } ] 都很正常
     * （"fix {bug}"、"数组越界 [i]"）。
     * 原实现只看字符本身，于是：
     *   - 标题里有 { → depth 提前归零 → end 落在标题中间，
     *     后面的 project/elapsed 全解析成空；
     *   - 标题里有 ] → 直接判定"数组结束"，整行会话凭空消失。
     * 字符串内的括号是文本，不参与结构计数。 */
    bool in_str = false, esc = false;

    while (*p) {
        char c = *p;

        if (in_str) {
            /* 字符串内部：只有 \" 是转义，其它字符都是文本 */
            if (esc)          esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  in_str = false;
            p++;
            continue;
        }

        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            if (depth == 0) {
                idx++;
                if (idx == n) obj_start = p;
            }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && obj_start) {
                *start = obj_start;
                *end   = p;
                return true;
            }
        } else if (c == ']' && depth == 0) {
            return false;
        }
        p++;
    }
    return false;
}

void board_parse(board_data_t *d, const char *json)
{
    memset(d, 0, sizeof(*d));
    d->ok = (strstr(json, "\"ok\":true") != NULL);
    if (!d->ok) json_str(json, "error", d->error, sizeof(d->error));

    json_str(json, "clock", d->clock, sizeof(d->clock));
    json_str(json, "date",  d->date,  sizeof(d->date));
    // 缺省 -1 表示"服务端没下发"，页脚显示 --，而不是假装"刚刚更新"
    d->age = json_int(json, "age", -1);
    d->working     = json_int(json, "working", 0);
    d->today_done  = json_int(json, "today_done", 0);
    d->automations = json_int(json, "automations", 0);
    // 缺省 -1 表示"服务端没下发"，页脚显示 --，不与真实的 0% 混为一谈
    d->cpu  = json_int(json, "cpu",  -1);
    d->mem  = json_int(json, "mem",  -1);
    d->disk = json_int(json, "disk", -1);
    d->temp = json_int(json, "temp",  0);
    // 芯片结温：没报过服务端给 -1，页脚显示 "--"，不显示假 0°C
    d->chip_temp = json_int(json, "chip_temp", -1);
    // 屏数：/status 和 /screen 都下发 nscr，固件据此决定按几屏循环
    d->nscr = json_int(json, "nscr", 0);

    // ---- 详情屏：页标题 + rows 数组 ----
    json_str(json, "t", d->page_title, sizeof(d->page_title));
    for (int i = 0; i < MAX_ROWS; i++) {
        const char *start = NULL, *end = NULL;
        if (!json_nth_object(json, "rows", i, &start, &end)) break;

        /* ⚠ 必须和 sessions 那段一样用 512。
         * 原来这里是 256，同一份数据在 1514 行用 sub[512]。当前字段量
         * （40+20+12 字节中文标题）撑不到 256，但服务端一加字段就静默截断 ——
         * 截断后 json_str 找不到键 → 显示空白，不崩、不报错，很难发现。 */
        char sub[512];
        size_t len = (size_t)(end - start) + 1;
        if (len >= sizeof(sub)) len = sizeof(sub) - 1;
        memcpy(sub, start, len);
        sub[len] = '\0';

        json_str(sub, "t", d->rows[i].t, sizeof(d->rows[i].t));
        json_str(sub, "v", d->rows[i].v, sizeof(d->rows[i].v));
        int c = json_int(sub, "c", 0);
        d->rows[i].color = (uint8_t)(c < 0 ? 0 : (c > 4 ? 4 : c));
        d->row_count++;
    }

    // ---- 图表屏：kind + charts 数组 + 底部两行 ----
    // kind 缺失 = 老服务端，is_chart 保持 false，走文本渲染，不会白屏。
    char kind[8];
    d->is_chart = json_str(json, "kind", kind, sizeof(kind))
                  && strcmp(kind, "chart") == 0;
    if (d->is_chart) {
        for (int i = 0; i < MAX_CHARTS; i++) {
            const char *start = NULL, *end = NULL;
            if (!json_nth_object(json, "charts", i, &start, &end)) break;

            // charts 对象比 rows 小得多（约 40 字节），128 足够。
            // 故意不用 512：这个数组每屏最多 3 个，省栈。
            char sub[128];
            size_t len = (size_t)(end - start) + 1;
            if (len >= sizeof(sub)) len = sizeof(sub) - 1;
            memcpy(sub, start, len);
            sub[len] = '\0';

            json_str(sub, "k", d->charts[i].k, sizeof(d->charts[i].k));
            json_str(sub, "n", d->charts[i].n, sizeof(d->charts[i].n));
            int v = json_int(sub, "v", 0);
            d->charts[i].v = (v < 0) ? 0 : (v > 100 ? 100 : v);
            int c = json_int(sub, "c", 0);
            d->charts[i].color = (uint8_t)(c < 0 ? 0 : (c > 5 ? 5 : c));
            d->chart_count++;
        }
        json_str(json, "f1", d->foot[0], sizeof(d->foot[0]));
        json_str(json, "f2", d->foot[1], sizeof(d->foot[1]));
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        const char *start = NULL, *end = NULL;
        if (!json_nth_object(json, "sessions", i, &start, &end)) break;

        char sub[512];
        size_t len = (size_t)(end - start) + 1;
        if (len >= sizeof(sub)) len = sizeof(sub) - 1;
        memcpy(sub, start, len);
        sub[len] = '\0';

        json_str(sub, "title",   d->sessions[i].title,   sizeof(d->sessions[i].title));
        json_str(sub, "project", d->sessions[i].project, sizeof(d->sessions[i].project));
        json_str(sub, "elapsed", d->sessions[i].elapsed, sizeof(d->sessions[i].elapsed));
        // e = 距最近活动的原始秒数，固件据此本地每秒累加。
        // 缺省 -1 表示"服务端没发"，此时回退到 elapsed 那个成品串，
        // 与真实的 0s（刚刚活动过）区分开
        int e = json_int(sub, "e", -1);
        d->sessions[i].age_s = (e < 0) ? -1 : e;
        // w 只在等待操作时下发，缺省即 0
        d->sessions[i].waiting = (uint8_t)json_int(sub, "w", 0);
        // ---- 实时状态（可选键）----
        // st: 0空闲 1运行中 2等你确认。服务端不发时按 0 处理，
        // 行为跟旧固件完全一样。
        int st = json_int(sub, "st", 0);
        d->sessions[i].state = (uint8_t)((st >= 0 && st <= 2) ? st : 0);
        // act: 当前动作的中文名，空表示没有实时信息
        json_str(sub, "act", d->sessions[i].action, sizeof(d->sessions[i].action));
        d->session_count++;
    }

    // 计时锚点：这批数据被解析时的本地时刻。会话年龄是相对服务端 now 的，
    // 加上从这一刻起流逝的时间 = 此刻的年龄。放在 memset 之后设，否则会被清掉。
    d->sample_ms = esp_timer_get_time() / 1000;
}
