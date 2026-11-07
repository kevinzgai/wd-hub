// passport-fw/main/wb/wd_core.h —— wb_view.c 的只读状态快照（适配层用的形状）。
//
// 从另一工程 passport-workbuddy/main/console/wb_core.h 摘出**纯类型部分**：
// 那边 wb_view.c 消费 wb_state_t，本工程业务层给的是 wb_view_t（wb_biz.h）。
// 为了让 1019 行视觉层原样复用、一行不改，这里保留 wb_state_t 的完整形状，
// 由 wb_ui.cc 里的 adapt_view() 做一次字段搬运。
//
// 本工程不链接 wb_core.c：状态机规则留在 wb_biz.c，这里只提供类型和
// wb_view.c 唯一用到的 wb_core_running_count()（内联实现）。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "wb_proto.h"
#include "wb_link_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 今日消耗页：按模型分布（规范 §6.3，最多 5 行）----
// WB_USAGE_MODELS / WB_USAGE_NAME_LEN / wb_usage_model_t 已下沉到 wb_proto.h
// （那是协议契约的一部分：主机 models[] 的容量上限）。本文件 include 它即可，
// 不再重复定义，否则会与协议头冲突。
typedef struct {
    bool valid;
    int64_t today;
    int req;
    int64_t cached;
    int n_model;
    wb_usage_model_t model[WB_USAGE_MODELS];
} wb_usage_t;

typedef enum {
    WB_BEEP_ASK = 0,
    WB_BEEP_APPROVAL,
    WB_BEEP_DONE,
    WB_BEEP_KIND_COUNT,
} wb_beep_kind_t;

typedef struct {
    bool valid;
    double remain;
    double total;
    uint8_t level;      // 0 主色 / 1 橙 / 2 红
} wb_credit_t;

typedef struct {
    bool active;
    int id;
    char tool[WB_TOOL_LEN];
    char hint[WB_HINT_LEN];
    int64_t deadline_ms;
} wb_approval_t;

typedef struct {
    bool active;
    int id;
    int ans;
    int multi;
    char tool[WB_TOOL_LEN];
    char text[WB_TEXT_LEN];
    int n_opts;
    char opt[WB_OPT_MAX][WB_OPT_LEN];
    int cursor;
    unsigned mask;
    int64_t deadline_ms;
} wb_ask_t;

typedef struct {
    wb_credit_t credit;

    wb_sess_t sess[WB_MAX_SESS];
    int n_sess;
    int count;

    wb_approval_t appr;
    wb_ask_t ask;

    bool linked;
    bool offline;
    int64_t last_rx_ms;

    bool rack_page;
    bool run_page;
    bool usage_page;
    bool run_manual;
    bool auto_suppressed;
    bool run_seen;
    int64_t run_start_ms;
    int64_t run_idle_since;

    bool standby_page;
    int64_t idle_since_ms;

    // 主机墙钟快照：clock_local_s <= 0 表示还没校时。
    int64_t clock_local_s;
    int64_t clock_sync_ms;

    wb_usage_t usage;

    // 主机资源页（第 5 页）：电脑侧 CPU / 内存 / 磁盘占用。
    // has_res=false（主机还没推 hostres 键）时整页显示说明文案，不画空条。
    bool has_res;
    wb_hostres_t res;

    // 网关 / 主机大盘（host 帧）：关于页「网关」卡的数据源。
    // has_host=false 表示主机从没发过 host 帧（老主机 / 还没连过），
    // 界面写「未下发」，不把 0 当成「网关没起来」。
    bool has_host;
    int gw_ports;        // 候选端口数（本地 worker 监听数）
    int gw_sessions;     // 网关侧活会话数
    int uptime_s;        // 主机端连接时长（秒）
    bool gw_secret;      // 网关密钥可读 = 本地网关在线
    bool gw_cli;         // CLI 可用

    // 链路诊断（设备侧自采，与主机版本无关）：收帧 / 发帧 / 断连次数与
    // 原因 / 连接间隔 / MTU / 省电档。
    wb_link_stats_t link;

    int beep_vol;
    uint8_t skin;
    int soc;

    int64_t boot_ms;
    int64_t last_beep_ms[WB_BEEP_KIND_COUNT];
} wb_state_t;

static inline int wb_core_running_count(const wb_state_t *st)
{
    int n = 0;
    for (int i = 0; i < st->n_sess; i++) {
        if (strcmp(st->sess[i].state, "RUNNING") == 0) n++;
    }
    return n;
}

#ifdef __cplusplus
}
#endif
