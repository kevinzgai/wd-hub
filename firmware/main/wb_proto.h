// main/wb_proto.h —— WorkBuddy 伴生控制台的帧协议(主机 ⇄ 设备)。
//
// 契约来源:需求规格 v1.1 §6.BLE 单帧 ≤512B,因此这里的容量常量必须与主机侧
// wb_app.py 的截断常量一致(ASK_TEXT_MAX=120 / ASK_OPT_LEN=31 / ASK_OPT_MAX=6)。
// 主机已按 UTF-8 边界预截断,设备这层只做"别再放大"的兜底截断。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 本工程的其余代码是 C++;这些是纯 C 模块,需要 C 链接。
#ifdef __cplusplus
extern "C" {
#endif

// ---- 容量(与主机侧契约对齐) ----
#define WB_MAX_SESS      3      // 单帧最多 3 条会话卡
#define WB_STATE_LEN     12     // RUNNING / NEED_ANSWER / ...
#define WB_MODEL_LEN     32     // 主机侧 MODEL_MAX=30，这里留 2B 余量
                                // （原来 24 会把 claude-sonnet-4-5-20250929 这类名字截断）
#define WB_NAME_LEN      48
#define WB_MSG_LEN       64
#define WB_HINT_LEN      256    // 审批命令原文
#define WB_TOOL_LEN      24
#define WB_TEXT_LEN      128
#define WB_OPT_MAX       6
#define WB_OPT_LEN       32
#define WB_RECV_MAX      512    // 设备接收缓冲(超长整帧丢弃)
#define WB_FW_VERSION    "1.2.0"
#define WB_PROTO_VERSION 2

// ---- 今日 token 消耗（第 7 屏）----
// 上设备的模型条数，必须与主机侧 wb_app.py 的 USAGE_MODELS_MAX 一致。
//
// 为什么是 5：屏幕内容区 188px，扣掉顶部两行 KPI（今日总量 + 请求/缓存）
// 每行 26px，剩 136px 给模型条；每条 22px（名字 + 占比条 + 数值）刚好 5 条
// 装满。第 6 条起在主机侧就并进"其它"，保证屏上数字之和 = 总量。
#define WB_USAGE_MODELS  5
// 模型名宽度，与主机 USAGE_MODEL_NAME_MAX 对齐（主机按字符预截）。
// 留 2 字符余量给主机可能补的省略号"…"或消歧后缀"·2"。
#define WB_USAGE_NAME_LEN 20

typedef struct {
    char name[WB_USAGE_NAME_LEN];
    int64_t tokens;      // 本模型今日 total_tokens
    int64_t cached;      // 本模型今日缓存命中 token
} wb_usage_model_t;

typedef struct {
    bool valid;          // 从未收到过 usage 帧时为 false，界面显示「--」
    int64_t today;       // 今日 total_tokens 合计
    int req;             // 今日请求数
    int64_t cached;      // 今日缓存命中 token
    int n_model;
    wb_usage_model_t model[WB_USAGE_MODELS];
} wb_usage_t;

// 能力位(需求 v1.1 §6.4)。本固件实现 bit0~bit2。
#define WB_CAP_APPROVAL_PAGE (1u << 0)
#define WB_CAP_ASK_PAGE      (1u << 1)
#define WB_CAP_ASK_MULTI     (1u << 2)
#define WB_CAP_COUNT_TOTAL   (1u << 3)   // 本固件不开:主机仍发"已发条数"
#define WB_CAP_AUTO_PAGE     (1u << 4)
#define WB_CAP_VOICE_UP      (1u << 5)

// 本固件**实际实现**的能力位。放在 wb_proto.h 而不是 wb_console.c：
// 主机据此决定要不要发 multi / 新的 count 语义，而核心层（wb_core.c）
// 也要用同一份真源做降级 —— 两边口径不一致就会出现"主机以为设备会多选、
// 设备却没法作答"的死页面。
//
// 单键板不声明 ASK_MULTI：多选需要"移动 / 勾选 / 提交"三种手势，
// 一颗按键表达不了。声明了反而会让主机推多选问题、设备却没法作答。
//
// 语音上行（bit5）已下线（2026-10-31 用户决定：暂时只做纯看板 + 审批/提问/
// 完成提醒）。不声明这一位，主机 _voice_loop 会自动把轮询降到探测档，
// 设备也不会再进发语音页。
#define WB_CAPS (WB_CAP_APPROVAL_PAGE | WB_CAP_ASK_PAGE)

typedef struct {
    char state[WB_STATE_LEN];
    char model[WB_MODEL_LEN];
    char name[WB_NAME_LEN];
    char msg[WB_MSG_LEN];
    double credit;
} wb_sess_t;

typedef enum {
    WB_EV_NONE = 0,
    WB_EV_STATUS,     // status_list
    WB_EV_CREDIT,     // {"remain":x,"total":y}(无 type 键,靠键名识别)
    WB_EV_APPROVAL,   // approval
    WB_EV_ASK,        // ask
    WB_EV_ASK_END,    // ask_end
    WB_EV_CFG,        // cfg
    WB_EV_MSG,        // msg(投递回执。语音已下线,保留识别能力防 ERR_JSON)
    WB_EV_USAGE,      // {"usage":1,...}(无 type 键,靠 usage 键识别)
} wb_ev_type_t;

typedef struct {
    wb_ev_type_t type;

    // 下行:
    int count;                       // status_list.count(主机当前发的是已发条数)
    int n_sess;
    wb_sess_t sess[WB_MAX_SESS];

    /* status_list.t —— 主机本地墙钟(epoch 秒，已按主机时区平移)。
     * 设备没有 RTC 电池、纯 BLE 也拿不到 NTP，待机页的大号时间只能由主机
     * 每 2s 随状态帧带下来。旧固件忽略这个键；旧主机不带它时设备显示
     * 「待机」而不是瞎猜一个时间。 */
    int64_t clock_s;

    double remain, total;            // 积分

    int approval_id;                 // approval.id
    char tool[WB_TOOL_LEN];
    char hint[WB_HINT_LEN];

    int ask_id;                      // ask.id
    int ask_ans;                     // 0 = 只读(请在电脑上回答)
    int ask_multi;                   // 1 = 多选(mask 回传)
    char text[WB_TEXT_LEN];
    int n_opts;
    char opt[WB_OPT_MAX][WB_OPT_LEN];

    int beep_vol;                    // cfg.beep_vol
    bool beep_test;                  // cfg.beep_test

    // 今日消耗（usage 帧）。注意 ev 是栈上结构，wb_usage_t 约
    // 5*(20+16) = 180 字节，加进 ev 后整个 ev 约 1.3KB —— 解码调用方
    // （wb_console 的 console_task 栈有 8KB）放得下，但不能把它挪到
    // 每帧都调的小栈函数里。
    wb_usage_t usage;
} wb_ev_t;

// 解析一帧下行数据。成功返回 true 并填 ev->type;
// 语法错误 / 未知 type / 缺关键字段返回 false(调用方回 ERR_JSON)。
bool wb_proto_decode(const char *frame, size_t len, wb_ev_t *ev);

// ---- 上行编码(返回写入的字节数,0 表示缓冲区不够) ----
size_t wb_proto_enc_key(char *out, size_t cap, int approval_id, const char *key);
size_t wb_proto_enc_ask_index(char *out, size_t cap, int ask_id, int index);
size_t wb_proto_enc_ask_mask(char *out, size_t cap, int ask_id, unsigned mask);
size_t wb_proto_enc_msg_target(char *out, size_t cap, const char *target);
size_t wb_proto_enc_battery(char *out, size_t cap, int soc);
size_t wb_proto_enc_hello(char *out, size_t cap, unsigned caps);

// 帧级应答(裸文本,非 JSON)。与主机 wb_app.py 的 ACK 处理对应。
const char *wb_proto_ack_for(wb_ev_type_t type, char *scratch, size_t cap, int beep_vol);

#ifdef __cplusplus
}  // extern "C"
#endif
