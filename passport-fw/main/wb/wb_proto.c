// passport-fw/main/wb/wb_proto.c —— 帧解码/编码（cJSON 实现，独立于 firmware/）。
#include "wb_proto.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static void copy_str(const cJSON *item, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!cJSON_IsString(item) || !item->valuestring) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s", item->valuestring);
}

// snprintf 按**字节**截断，会把多字节 UTF-8 字符切成半个。
// LVGL 拿到非法序列会画出方块或直接吞掉整行，比截短更难排查。
// 这里只做一件事：若结尾那个字符不完整，就把它整个砍掉。
static void utf8_trim_incomplete(char *s)
{
    if (!s) return;
    const size_t n = strlen(s);
    if (n == 0) return;

    size_t i = n;                                  // 跳到最后一个字符的首字节
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) { s[0] = '\0'; return; }            // 全是续字节，整串作废

    const unsigned char lead = (unsigned char)s[i - 1];
    size_t need = 1;
    if (lead >= 0xF0) need = 4;
    else if (lead >= 0xE0) need = 3;
    else if (lead >= 0xC0) need = 2;

    if (n - (i - 1) < need) s[i - 1] = '\0';        // 尾部字符被截断，丢掉
}

static int json_int(const cJSON *root, const char *key, int def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(it) ? (int)it->valuedouble : def;
}

static int64_t json_i64(const cJSON *root, const char *key, int64_t def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(it) ? (int64_t)it->valuedouble : def;
}

static double json_double(const cJSON *root, const char *key, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

// cJSON_IsTrue() 只认 JSON 字面量 true，对 1 / "1" 一律返回 false。
// 而 cfg.beep_test 在主机侧的早期实现里就是数字 1（另一条固件线的
// wb_json_bool 把「非 0 数字」当 true，所以那里一直正常）。移植时直接换成
// cJSON_IsTrue，试听标志就被静默吞掉了。这里按「真值」语义兼容三种写法，
// 让新旧主机下发的 cfg 都能被正确识别。
static bool json_bool(const cJSON *root, const char *key, bool def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!it) return def;
    if (cJSON_IsTrue(it)) return true;
    if (cJSON_IsFalse(it)) return false;
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    if (cJSON_IsString(it) && it->valuestring) {
        return strcmp(it->valuestring, "1") == 0 ||
               strcmp(it->valuestring, "true") == 0;
    }
    return def;
}

static void decode_status(const cJSON *root, wb_ev_t *ev)
{
    ev->count = json_int(root, "count", 0);
    ev->clock_s = json_i64(root, "t", 0);

    const cJSON *tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    ev->n_sess = 0;
    if (!cJSON_IsArray(tasks)) return;   // 空列表合法

    const int total = cJSON_GetArraySize(tasks);
    for (int i = 0; i < total && ev->n_sess < WB_MAX_SESS; ++i) {
        const cJSON *it = cJSON_GetArrayItem(tasks, i);
        if (!cJSON_IsObject(it)) continue;
        wb_sess_t *s = &ev->sess[ev->n_sess];
        memset(s, 0, sizeof(*s));
        copy_str(cJSON_GetObjectItemCaseSensitive(it, "state"), s->state, sizeof(s->state));
        copy_str(cJSON_GetObjectItemCaseSensitive(it, "model"), s->model, sizeof(s->model));
        copy_str(cJSON_GetObjectItemCaseSensitive(it, "name"), s->name, sizeof(s->name));
        copy_str(cJSON_GetObjectItemCaseSensitive(it, "msg"), s->msg, sizeof(s->msg));
        const cJSON *credit = cJSON_GetObjectItemCaseSensitive(it, "credit");
        s->credit = cJSON_IsNumber(credit) ? credit->valuedouble : 0.0;
        if (s->state[0] == '\0') snprintf(s->state, sizeof(s->state), "IDLE");
        ev->n_sess++;
    }
}

static void decode_ask(const cJSON *root, wb_ev_t *ev)
{
    ev->ask_id = json_int(root, "id", 0);
    ev->ask_ans = json_int(root, "ans", 0);
    ev->ask_multi = json_int(root, "multi", 0);
    copy_str(cJSON_GetObjectItemCaseSensitive(root, "tool"), ev->tool, sizeof(ev->tool));
    copy_str(cJSON_GetObjectItemCaseSensitive(root, "text"), ev->text, sizeof(ev->text));

    ev->n_opts = 0;
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(root, "opts");
    const int total = cJSON_GetArraySize(opts);
    for (int i = 0; i < total && ev->n_opts < WB_OPT_MAX; ++i) {
        copy_str(cJSON_GetArrayItem(opts, i), ev->opt[ev->n_opts], WB_OPT_LEN);
        ev->n_opts++;
    }
    if (ev->n_opts == 0) ev->ask_ans = 0;   // 没有选项就按只读处理
}

bool wb_proto_decode(const char *frame, size_t len, wb_ev_t *ev)
{
    if (!frame || !ev || len == 0 || len > WB_RECV_MAX) return false;

    cJSON *root = cJSON_ParseWithLength(frame, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    memset(ev, 0, sizeof(*ev));
    bool ok = true;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring || type->valuestring[0] == '\0') {
        // 无 type 的帧：voice 轮询 / usage / 积分。前两者本固件不消费，
        // 但必须识别以免主机每 0.25s 收到一条 ERR_JSON。
        if (cJSON_GetObjectItemCaseSensitive(root, "voice")) {
            ev->type = WB_EV_NONE;
        } else if (cJSON_GetObjectItemCaseSensitive(root, "usage")) {
            // 主机每 30s 推一次今日用量；此前只识别不解析，数据全丢了。
            ev->type = WB_EV_USAGE;
            ev->usage_today = json_double(root, "today", 0.0);
            ev->usage_cache = json_double(root, "cache", 0.0);
            ev->usage_req = json_int(root, "req", 0);
            const cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
            // models[] 形如 [{"n":"deepseek-v4.1-f…","v":123456,"hit":789}, ...]
            // 主机 wb_app.py 已按用量降序、最多 5 项（TOP4 + 「其它」），
            // 名字也按 16 字符预截断；这里只做容量兜底，不重排。
            // 以前这里只取 cJSON_GetArraySize()，名字与 token 数全丢，
            // 「模型用量」卡片的 5 行进度条因此永远是空的。
            ev->usage_models = 0;
            if (cJSON_IsArray(models)) {
                const cJSON *it = NULL;
                cJSON_ArrayForEach(it, models) {
                    if (ev->usage_models >= WB_USAGE_MODELS) break;
                    if (!cJSON_IsObject(it)) continue;
                    wb_usage_model_t *m = &ev->usage_model[ev->usage_models];
                    copy_str(cJSON_GetObjectItemCaseSensitive(it, "n"),
                             m->name, sizeof(m->name));
                    utf8_trim_incomplete(m->name);
                    // 没名字的行直接跳过：宁可少一行，也不要一条无标签空条。
                    if (m->name[0] == '\0') continue;
                    m->tokens = json_i64(it, "v", 0);
                    m->cached = json_i64(it, "hit", 0);
                    ev->usage_models++;
                }
            }
        } else if (cJSON_GetObjectItemCaseSensitive(root, "host")) {
            // Web 大盘的主机侧信息：status_list 已 488B 塞不下，走独立小帧。
            ev->type = WB_EV_HOST;
            ev->gw_ports = json_int(root, "gw_ports", 0);
            ev->gw_sessions = json_int(root, "gw_sessions", 0);
            ev->uptime_s = json_int(root, "uptime", 0);
            ev->gw_secret = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "secret"));
            ev->gw_cli = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "cli"));
        } else if (cJSON_HasObjectItem(root, "remain")) {
            ev->type = WB_EV_CREDIT;
            ev->remain = cJSON_GetObjectItemCaseSensitive(root, "remain")->valuedouble;
            ev->total = json_int(root, "total", 0);
        } else {
            ok = false;
        }
        cJSON_Delete(root);
        return ok;
    }

    const char *t = type->valuestring;
    if (strcmp(t, "status_list") == 0) {
        ev->type = WB_EV_STATUS;
        decode_status(root, ev);
    } else if (strcmp(t, "approval") == 0) {
        ev->type = WB_EV_APPROVAL;
        ev->approval_id = json_int(root, "id", 0);
        copy_str(cJSON_GetObjectItemCaseSensitive(root, "tool"), ev->tool, sizeof(ev->tool));
        copy_str(cJSON_GetObjectItemCaseSensitive(root, "hint"), ev->hint, sizeof(ev->hint));
    } else if (strcmp(t, "ask") == 0) {
        ev->type = WB_EV_ASK;
        decode_ask(root, ev);
    } else if (strcmp(t, "ask_end") == 0) {
        ev->type = WB_EV_ASK_END;
        ev->ask_id = json_int(root, "id", 0);
    } else if (strcmp(t, "cfg") == 0) {
        ev->type = WB_EV_CFG;
        ev->beep_vol = json_int(root, "beep_vol", -1);
        ev->beep_test = json_bool(root, "beep_test", false);
        ok = ev->beep_vol >= 0;
    } else if (strcmp(t, "msg") == 0) {
        ev->type = WB_EV_MSG;
    } else {
        ok = false;   // 未知 type：回 ERR_JSON，便于主机日志定位固件版本
    }

    cJSON_Delete(root);
    return ok;
}

static size_t finish(char *out, size_t cap, int n)
{
    if (n < 0 || (size_t)n >= cap) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

size_t wb_proto_enc_key(char *out, size_t cap, int approval_id, const char *key)
{
    if (!out || cap == 0 || !key) return 0;
    return finish(out, cap,
                  snprintf(out, cap, "{\"type\":\"key\",\"approval_id\":%d,\"key\":\"%s\"}\n",
                           approval_id, key));
}

size_t wb_proto_enc_ask_index(char *out, size_t cap, int ask_id, int index)
{
    if (!out || cap == 0) return 0;
    return finish(out, cap,
                  snprintf(out, cap, "{\"type\":\"ask_answer\",\"id\":%d,\"index\":%d}\n",
                           ask_id, index));
}

size_t wb_proto_enc_ask_mask(char *out, size_t cap, int ask_id, unsigned mask)
{
    if (!out || cap == 0) return 0;
    return finish(out, cap,
                  snprintf(out, cap, "{\"type\":\"ask_answer\",\"id\":%d,\"mask\":%u}\n",
                           ask_id, mask));
}

size_t wb_proto_enc_battery(char *out, size_t cap, int soc)
{
    if (!out || cap == 0) return 0;
    return finish(out, cap, snprintf(out, cap, "{\"type\":\"battery\",\"soc\":%d}\n", soc));
}

size_t wb_proto_enc_hello(char *out, size_t cap, unsigned caps)
{
    if (!out || cap == 0) return 0;
    return finish(out, cap,
                  snprintf(out, cap,
                           "{\"type\":\"hello\",\"fw\":\"%s\",\"proto\":%d,\"caps\":%u}\n",
                           WB_FW_VERSION, WB_PROTO_VERSION, caps));
}

const char *wb_proto_ack_for(wb_ev_type_t type, char *scratch, size_t cap, int beep_vol)
{
    switch (type) {
    case WB_EV_STATUS: return "OK_STATUS";
    case WB_EV_CREDIT: return "OK_WB";
    case WB_EV_APPROVAL: return "OK_APPROVAL";
    case WB_EV_ASK: return "OK_ASK";
    case WB_EV_ASK_END: return "OK_ASK_END";
    case WB_EV_MSG: return "OK_MSG";
    case WB_EV_CFG:
        if (!scratch || cap == 0) return "OK_CFG";
        snprintf(scratch, cap, "OK_CFG:%d", beep_vol);
        return scratch;
    default: return NULL;
    }
}
