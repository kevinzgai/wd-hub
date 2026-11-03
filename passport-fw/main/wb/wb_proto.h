// passport-fw/main/wb/wb_proto.h —— WorkBuddy BLE 帧协议（本工程独立实现）。
//
// 与主机 host/wb_app.py 的契约与 S3 小智盒子一致，但本文件是重新实现的：
// 不包含、不链接 firmware/main/wb_proto.*，只复用相同的字段名与语义。
// 单帧 ≤512B；主机已按 UTF-8 边界预截断，本层只做容量兜底。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WB_MAX_SESS      3
#define WB_STATE_LEN     12
#define WB_MODEL_LEN     32
#define WB_NAME_LEN      48
#define WB_MSG_LEN       64
#define WB_HINT_LEN      256
#define WB_TOOL_LEN      24
#define WB_TEXT_LEN      128
#define WB_OPT_MAX       6
#define WB_OPT_LEN       32
#define WB_RECV_MAX      512
#define WB_FW_VERSION    "1.3.0-passport"
#define WB_PROTO_VERSION 2

// 能力位：本工程只做看板 + 审批 + 提问（与 S3 看板固件同一口径）。
#define WB_CAP_APPROVAL_PAGE (1u << 0)
#define WB_CAP_ASK_PAGE      (1u << 1)
#define WB_CAP_ASK_MULTI     (1u << 2)
#define WB_CAPS              (WB_CAP_APPROVAL_PAGE | WB_CAP_ASK_PAGE)

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
    WB_EV_CREDIT,     // {"remain":x,"total":y}
    WB_EV_APPROVAL,   // approval
    WB_EV_ASK,        // ask
    WB_EV_ASK_END,    // ask_end
    WB_EV_CFG,        // cfg
    WB_EV_MSG,        // msg（语音已下线，识别后安静吞掉）
    WB_EV_USAGE,      // {"usage":1,...}
} wb_ev_type_t;

typedef struct {
    wb_ev_type_t type;

    int count;
    int n_sess;
    wb_sess_t sess[WB_MAX_SESS];

    /* status_list.t —— 主机本地墙钟（epoch 秒，已按主机时区平移）。
     * 设备没有 RTC，待机页大号时间只能由主机每 2s 带下来。 */
    int64_t clock_s;

    double remain, total;

    int approval_id;
    char tool[WB_TOOL_LEN];
    char hint[WB_HINT_LEN];

    int ask_id;
    int ask_ans;      // 0 = 只读（请在电脑上回答）
    int ask_multi;
    char text[WB_TEXT_LEN];
    int n_opts;
    char opt[WB_OPT_MAX][WB_OPT_LEN];

    int beep_vol;
    bool beep_test;
} wb_ev_t;

bool wb_proto_decode(const char *frame, size_t len, wb_ev_t *ev);

size_t wb_proto_enc_key(char *out, size_t cap, int approval_id, const char *key);
size_t wb_proto_enc_ask_index(char *out, size_t cap, int ask_id, int index);
size_t wb_proto_enc_ask_mask(char *out, size_t cap, int ask_id, unsigned mask);
size_t wb_proto_enc_battery(char *out, size_t cap, int soc);
size_t wb_proto_enc_hello(char *out, size_t cap, unsigned caps);

const char *wb_proto_ack_for(wb_ev_type_t type, char *scratch, size_t cap, int beep_vol);

#ifdef __cplusplus
}
#endif
