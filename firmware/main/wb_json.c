// main/wb_json.c —— wb_json.h 的实现:递归下降 + 固定节点池。
#include "wb_json.h"

#include <stdlib.h>
#include <string.h>

static void skip_ws(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    *pp = p;
}

static int16_t alloc_node(wb_json_t *j, wb_json_type_t type)
{
    if (j->used >= WB_JSON_MAX_NODES) return -1;
    const int16_t idx = j->used++;
    wb_json_node_t *n = &j->node[idx];
    n->type = (uint8_t)type;
    n->kids = 0;
    n->pos = 0;
    n->len = 0;
    n->first = -1;
    n->next = -1;
    return idx;
}

// p 指向开引号。成功返回 true 并就地推进到闭引号之后。
static bool parse_string_raw(wb_json_t *j, const char **pp, const char *end,
                            uint16_t *pos, uint16_t *len)
{
    const char *p = *pp;
    if (p >= end || *p != '"') return false;
    p++;
    const char *start = p;
    while (p < end) {
        const char c = *p;
        if (c == '\\') {
            if (p + 1 >= end) return false;
            p += 2;
            continue;
        }
        if (c == '"') break;
        p++;
    }
    if (p >= end) return false;   // 没有闭合引号
    *pos = (uint16_t)(start - j->src);
    *len = (uint16_t)(p - start);
    *pp = p + 1;
    return true;
}

static bool is_num_char(char c)
{
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static int16_t parse_value(wb_json_t *j, const char **pp, const char *end, int depth);

static int16_t parse_array(wb_json_t *j, const char **pp, const char *end, int depth)
{
    const int16_t self = alloc_node(j, WB_JSON_ARR);
    if (self < 0) return -1;
    const char *p = *pp + 1;   // 吃掉 '['
    int16_t last = -1;
    for (;;) {
        skip_ws(&p, end);
        if (p >= end) return -1;
        if (*p == ']') { p++; break; }
        const int16_t child = parse_value(j, &p, end, depth + 1);
        if (child < 0) return -1;
        if (last < 0) j->node[self].first = child;
        else j->node[last].next = child;
        last = child;
        if (j->node[self].kids < 255) j->node[self].kids++;
        skip_ws(&p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; break; }
        return -1;
    }
    *pp = p;
    return self;
}

static int16_t parse_object(wb_json_t *j, const char **pp, const char *end, int depth)
{
    const int16_t self = alloc_node(j, WB_JSON_OBJ);
    if (self < 0) return -1;
    const char *p = *pp + 1;   // 吃掉 '{'
    int16_t last = -1;
    for (;;) {
        skip_ws(&p, end);
        if (p >= end) return -1;
        if (*p == '}') { p++; break; }
        const int16_t key = alloc_node(j, WB_JSON_STR);
        if (key < 0) return -1;
        if (!parse_string_raw(j, &p, end, &j->node[key].pos, &j->node[key].len)) return -1;
        skip_ws(&p, end);
        if (p >= end || *p != ':') return -1;
        p++;
        const int16_t val = parse_value(j, &p, end, depth + 1);
        if (val < 0) return -1;
        j->node[key].next = val;   // 键值成对:key.next 指向 value
        if (last < 0) j->node[self].first = key;
        else j->node[last].next = key;
        last = val;
        if (j->node[self].kids < 255) j->node[self].kids++;
        skip_ws(&p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') { p++; break; }
        return -1;
    }
    *pp = p;
    return self;
}

static int16_t parse_value(wb_json_t *j, const char **pp, const char *end, int depth)
{
    if (depth > WB_JSON_MAX_DEPTH) return -1;
    skip_ws(pp, end);
    const char *p = *pp;
    if (p >= end) return -1;

    if (*p == '{') return parse_object(j, pp, end, depth);
    if (*p == '[') return parse_array(j, pp, end, depth);

    if (*p == '"') {
        const int16_t n = alloc_node(j, WB_JSON_STR);
        if (n < 0) return -1;
        if (!parse_string_raw(j, &p, end, &j->node[n].pos, &j->node[n].len)) return -1;
        *pp = p;
        return n;
    }

    if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
        const int16_t n = alloc_node(j, WB_JSON_TRUE);
        if (n < 0) return -1;
        *pp = p + 4;
        return n;
    }
    if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
        const int16_t n = alloc_node(j, WB_JSON_FALSE);
        if (n < 0) return -1;
        *pp = p + 5;
        return n;
    }
    if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
        const int16_t n = alloc_node(j, WB_JSON_NULL);
        if (n < 0) return -1;
        *pp = p + 4;
        return n;
    }

    if (is_num_char(*p)) {
        const char *start = p;
        bool has_digit = false;
        while (p < end && is_num_char(*p)) {
            if (*p >= '0' && *p <= '9') has_digit = true;
            p++;
        }
        if (!has_digit) return -1;
        const int16_t n = alloc_node(j, WB_JSON_NUM);
        if (n < 0) return -1;
        j->node[n].pos = (uint16_t)(start - j->src);
        j->node[n].len = (uint16_t)(p - start);
        *pp = p;
        return n;
    }
    return -1;
}

bool wb_json_parse(wb_json_t *j, const char *text, size_t len)
{
    if (!j || !text) return false;
    if (len > 0xFFFF) return false;
    j->used = 0;
    j->src = text;
    j->src_len = (uint16_t)len;

    const char *p = text;
    const char *end = text + len;
    const int16_t root = parse_value(j, &p, end, 0);
    if (root < 0) return false;
    skip_ws(&p, end);
    if (p != end) return false;
    if (root != 0) return false;   // 根节点必须是第一个分配的节点
    return true;
}

int wb_json_root(const wb_json_t *j)
{
    if (!j || j->used <= 0) return -1;
    return 0;
}

wb_json_type_t wb_json_type(const wb_json_t *j, int n)
{
    if (!j || n < 0 || n >= j->used) return WB_JSON_NULL;
    return (wb_json_type_t)j->node[n].type;
}

bool wb_json_is_obj(const wb_json_t *j, int n) { return wb_json_type(j, n) == WB_JSON_OBJ; }
bool wb_json_is_arr(const wb_json_t *j, int n) { return wb_json_type(j, n) == WB_JSON_ARR; }

static bool slice_equals(const wb_json_t *j, int n, const char *s)
{
    if (!j || n < 0 || n >= j->used) return false;
    const wb_json_node_t *node = &j->node[n];
    if (node->type != WB_JSON_STR) return false;
    const size_t want = strlen(s);
    if (want != node->len) return false;
    return memcmp(j->src + node->pos, s, want) == 0;
}

int wb_json_obj_get(const wb_json_t *j, int obj, const char *key)
{
    if (!j || !key || !wb_json_is_obj(j, obj)) return -1;
    int key_node = j->node[obj].first;
    while (key_node >= 0) {
        if (slice_equals(j, key_node, key)) {
            const int val = j->node[key_node].next;
            return val;
        }
        // key.next 是 value,value.next 才是下一个键
        const int val = j->node[key_node].next;
        key_node = (val >= 0) ? j->node[val].next : -1;
    }
    return -1;
}

int wb_json_arr_len(const wb_json_t *j, int arr)
{
    if (!j || !wb_json_is_arr(j, arr)) return 0;
    return j->node[arr].kids;
}

int wb_json_arr_at(const wb_json_t *j, int arr, int index)
{
    if (!j || index < 0 || !wb_json_is_arr(j, arr)) return -1;
    int n = j->node[arr].first;
    for (int i = 0; i < index && n >= 0; i++) n = j->node[n].next;
    return n;
}

size_t wb_json_str(const wb_json_t *j, int n, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!j || n < 0 || n >= j->used) return 0;
    const wb_json_node_t *node = &j->node[n];
    if (node->type != WB_JSON_STR) return 0;

    const char *p = j->src + node->pos;
    const char *end = p + node->len;
    size_t o = 0;

    while (p < end) {
        if (o + 1 >= cap) break;   // 留 '\0'
        const char c = *p++;
        if (c != '\\') { out[o++] = c; continue; }
        if (p >= end) break;
        const char e = *p++;
        switch (e) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u':
            // 不猜:跳过 4 位十六进制,输出一个占位符。主机侧不下发 \u 转义。
            for (int k = 0; k < 4 && p < end; k++) p++;
            if (o + 1 < cap) out[o++] = '?';
            break;
        default: out[o++] = e; break;
        }
    }

    // 按 UTF-8 边界回退,避免把一个汉字的 3 字节切成半个(屏幕上会花屏)。
    // 注意:不能"见续字节就砍"——合法字符的结尾本来就是续字节。正确做法是先
    // 回到最后一个字符的首字节,再看它需要的总长度是否还放得下。
    size_t q = o;
    size_t i = q;
    while (i > 0 && ((unsigned char)out[i - 1] & 0xC0) == 0x80) i--;
    if (i > 0) {
        const unsigned char lead = (unsigned char)out[i - 1];
        const size_t need = (lead < 0x80) ? 1 : ((lead & 0xE0) == 0xC0) ? 2
                          : ((lead & 0xF0) == 0xE0) ? 3 : 4;
        if (i - 1 + need > q) q = i - 1;
    }
    out[q] = '\0';
    return q;
}

double wb_json_num(const wb_json_t *j, int n, double dflt)
{
    if (!j || n < 0 || n >= j->used) return dflt;
    const wb_json_node_t *node = &j->node[n];
    if (node->type != WB_JSON_NUM) return dflt;
    char tmp[32];
    const size_t len = node->len < sizeof(tmp) - 1 ? node->len : sizeof(tmp) - 1;
    memcpy(tmp, j->src + node->pos, len);
    tmp[len] = '\0';
    char *stop = NULL;
    const double v = strtod(tmp, &stop);
    if (stop == tmp) return dflt;
    return v;
}

int wb_json_int(const wb_json_t *j, int n, int dflt)
{
    if (!j || n < 0 || n >= j->used) return dflt;
    if (wb_json_type(j, n) != WB_JSON_NUM) return dflt;
    return (int)wb_json_num(j, n, (double)dflt);
}

bool wb_json_bool(const wb_json_t *j, int n, bool dflt)
{
    if (!j || n < 0 || n >= j->used) return dflt;
    switch (j->node[n].type) {
    case WB_JSON_TRUE: return true;
    case WB_JSON_FALSE: return false;
    case WB_JSON_NUM: return wb_json_num(j, n, 0) != 0.0;
    default: return dflt;
    }
}
