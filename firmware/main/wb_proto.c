// main/wb_proto.c —— 帧解码 + 上行编码。纯逻辑,不碰 BLE/LVGL,可 host 测试。
#include "wb_proto.h"

#include <stdio.h>
#include <string.h>

#include "wb_json.h"

// 解码期只有一个解析器实例在工作;放静态区而不是栈上——节点池 64×10B 加源切片,
// 落在 BLE host 任务的栈里没有意义(那任务还要跑 NimBLE)。
static wb_json_t s_json;

static void copy_str(const wb_json_t *j, int node, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (node < 0) { out[0] = '\0'; return; }
    wb_json_str(j, node, out, cap);
}

// 今日消耗帧解码。
//
// 契约：{"usage":1,"today":N,"req":N,"cache":N,
//        "models":[{"n":"名","v":N,"hit":N}, ...]}
//
// 三个要点：
//   1) 没有 "type" 键，靠顶层 "usage" 键识别（与积分帧同一惯例）——
//      带了 "type":"usage" 反而会掉进 type 分发里无人处理。
//   2) 所有数值用 wb_json_num 转 int64，不用 wb_json_int：后者是 int，
//      几百万 token 没事，但用户跑满一个月/批量导入时可能到 2^31。
//      double 有 53 位尾数，精确表示到 9e15，够用。
//   3) models 数超过 WB_USAGE_MODELS 时**只取前 N 条，不合并**——
//      合并是主机侧的职责（它保证前 N-1 条 + "其它"之和 == today）。
//      设备这边硬截是为了不让脏帧写越界，截了之后屏上数字之和对不上
//      总量属于主机违约，不是固件要兜的。
static bool decode_usage(const wb_json_t *j, int root, wb_ev_t *ev)
{
    wb_usage_t *u = &ev->usage;
    memset(u, 0, sizeof(*u));

    u->today  = (int64_t)wb_json_num(j, wb_json_obj_get(j, root, "today"), 0.0);
    u->req    = wb_json_int(j, wb_json_obj_get(j, root, "req"), 0);
    u->cached = (int64_t)wb_json_num(j, wb_json_obj_get(j, root, "cache"), 0.0);

    const int arr = wb_json_obj_get(j, root, "models");
    u->n_model = 0;
    if (wb_json_is_arr(j, arr)) {
        const int total = wb_json_arr_len(j, arr);
        for (int i = 0; i < total && u->n_model < WB_USAGE_MODELS; i++) {
            const int it = wb_json_arr_at(j, arr, i);
            if (!wb_json_is_obj(j, it)) continue;
            wb_usage_model_t *m = &u->model[u->n_model];
            copy_str(j, wb_json_obj_get(j, it, "n"), m->name, sizeof(m->name));
            m->tokens = (int64_t)wb_json_num(j, wb_json_obj_get(j, it, "v"), 0.0);
            m->cached = (int64_t)wb_json_num(j, wb_json_obj_get(j, it, "hit"), 0.0);
            // 空名字的条目直接跳过：屏上会画出一条没有标签的横杠，
            // 看着像渲染 bug。主机正常不会发空名，这里是防脏帧。
            if (m->name[0] != '\0') u->n_model++;
        }
    }

    // 一条模型都没有也是合法的（今天有请求但都没标模型），
    // 此时屏上只显示总量，不画条形 —— 不算解码失败。
    u->valid = true;
    return true;
}

static bool decode_status(const wb_json_t *j, int root, wb_ev_t *ev)
{
    ev->count = wb_json_int(j, wb_json_obj_get(j, root, "count"), 0);
    const int tasks = wb_json_obj_get(j, root, "tasks");
    ev->n_sess = 0;
    if (!wb_json_is_arr(j, tasks)) return true;   // 空列表是合法的

    const int total = wb_json_arr_len(j, tasks);
    for (int i = 0; i < total && ev->n_sess < WB_MAX_SESS; i++) {
        const int t = wb_json_arr_at(j, tasks, i);
        if (!wb_json_is_obj(j, t)) continue;
        wb_sess_t *s = &ev->sess[ev->n_sess];
        memset(s, 0, sizeof(*s));
        copy_str(j, wb_json_obj_get(j, t, "state"), s->state, sizeof(s->state));
        copy_str(j, wb_json_obj_get(j, t, "model"), s->model, sizeof(s->model));
        copy_str(j, wb_json_obj_get(j, t, "name"), s->name, sizeof(s->name));
        copy_str(j, wb_json_obj_get(j, t, "msg"), s->msg, sizeof(s->msg));
        s->credit = wb_json_num(j, wb_json_obj_get(j, t, "credit"), 0.0);
        if (s->state[0] == '\0') snprintf(s->state, sizeof(s->state), "IDLE");
        ev->n_sess++;
    }
    return true;
}

static bool decode_ask(const wb_json_t *j, int root, wb_ev_t *ev)
{
    ev->ask_id = wb_json_int(j, wb_json_obj_get(j, root, "id"), 0);
    ev->ask_ans = wb_json_int(j, wb_json_obj_get(j, root, "ans"), 0);
    ev->ask_multi = wb_json_int(j, wb_json_obj_get(j, root, "multi"), 0);
    copy_str(j, wb_json_obj_get(j, root, "tool"), ev->tool, sizeof(ev->tool));
    copy_str(j, wb_json_obj_get(j, root, "text"), ev->text, sizeof(ev->text));

    ev->n_opts = 0;
    const int opts = wb_json_obj_get(j, root, "opts");
    const int total = wb_json_arr_len(j, opts);
    for (int i = 0; i < total && ev->n_opts < WB_OPT_MAX; i++) {
        copy_str(j, wb_json_arr_at(j, opts, i), ev->opt[ev->n_opts], WB_OPT_LEN);
        ev->n_opts++;
    }
    // 没有可选项就没有可作答的页面:按只读处理,避免给出一个空列表。
    if (ev->n_opts == 0) ev->ask_ans = 0;
    return true;
}

bool wb_proto_decode(const char *frame, size_t len, wb_ev_t *ev)
{
    if (!frame || !ev || len == 0) return false;
    if (len > WB_RECV_MAX) return false;          // 设备侧硬上限:超长整帧丢弃
    if (!wb_json_parse(&s_json, frame, len)) return false;

    const wb_json_t *j = &s_json;
    const int root = wb_json_root(j);
    if (!wb_json_is_obj(j, root)) return false;

    memset(ev, 0, sizeof(*ev));

    char type[20];
    copy_str(j, wb_json_obj_get(j, root, "type"), type, sizeof(type));

    if (type[0] == '\0') {
        // 语音轮询帧 {"voice":"poll"} 是发给"有麦克风能力"的固件当触发器的,
        // 本固件不出声也不录,必须安静忽略——若按未知帧回 ERR_JSON,主机会每
        // 0.25s 记一条错误,日志立刻被刷爆。
        if (wb_json_obj_get(j, root, "voice") >= 0) {
            ev->type = WB_EV_NONE;
            return true;
        }
        // 今日消耗帧同样没有 type 键（与积分帧一致，见 wb_proto.h 的
        // WB_EV_USAGE 注释）。必须在 remain 之前判：usage 帧不带 remain，
        // 先判 remain 会把它当未知帧回 ERR_JSON，主机每 30s 记一条错误。
        if (wb_json_obj_get(j, root, "usage") >= 0) {
            ev->type = WB_EV_USAGE;
            return decode_usage(j, root, ev);
        }
        // 积分帧没有 type 键,靠 remain/total 识别(契约如此)。
        const int remain = wb_json_obj_get(j, root, "remain");
        if (remain < 0) return false;
        ev->type = WB_EV_CREDIT;
        ev->remain = wb_json_num(j, remain, 0.0);
        ev->total = wb_json_num(j, wb_json_obj_get(j, root, "total"), 0.0);
        return true;
    }

    if (strcmp(type, "status_list") == 0) {
        ev->type = WB_EV_STATUS;
        return decode_status(j, root, ev);
    }
    if (strcmp(type, "approval") == 0) {
        ev->type = WB_EV_APPROVAL;
        ev->approval_id = wb_json_int(j, wb_json_obj_get(j, root, "id"), 0);
        copy_str(j, wb_json_obj_get(j, root, "tool"), ev->tool, sizeof(ev->tool));
        copy_str(j, wb_json_obj_get(j, root, "hint"), ev->hint, sizeof(ev->hint));
        return true;
    }
    if (strcmp(type, "ask") == 0) {
        ev->type = WB_EV_ASK;
        return decode_ask(j, root, ev);
    }
    if (strcmp(type, "ask_end") == 0) {
        ev->type = WB_EV_ASK_END;
        ev->ask_id = wb_json_int(j, wb_json_obj_get(j, root, "id"), 0);
        return true;
    }
    if (strcmp(type, "cfg") == 0) {
        ev->type = WB_EV_CFG;
        ev->beep_vol = wb_json_int(j, wb_json_obj_get(j, root, "beep_vol"), -1);
        ev->beep_test = wb_json_bool(j, wb_json_obj_get(j, root, "beep_test"), false);
        return ev->beep_vol >= 0;
    }
    if (strcmp(type, "msg") == 0) {
        // 语音已下线，但主机升级过渡期可能仍发回执帧。能识别就不回
        // ERR_JSON，安静吞掉即可。
        ev->type = WB_EV_MSG;
        return true;
    }
    return false;   // 不认识的 type:回 ERR_JSON,让主机日志能指向固件版本
}

// ---- 编码 ----
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

size_t wb_proto_enc_msg_target(char *out, size_t cap, const char *target)
{
    if (!out || cap == 0 || !target) return 0;
    return finish(out, cap,
                  snprintf(out, cap, "{\"type\":\"msg_target\",\"target\":\"%s\"}\n", target));
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

