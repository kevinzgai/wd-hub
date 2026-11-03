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
            ev->type = WB_EV_USAGE;
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
        ev->beep_test = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "beep_test"));
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
