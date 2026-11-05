// passport-fw/main/wb/wb_biz.h —— WB 业务状态机（纯数据 + BLE，不碰 LVGL）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wb_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_PAGE_OVERVIEW = 0,
    WB_PAGE_APPROVAL,
    WB_PAGE_ASK,
} wb_page_t;

// 唤醒原因位掩码：业务层只负责“发生了什么”，何时点亮/退出待机由 wb_power 决定。
#define WB_WAKE_TASK     (1u << 0)
#define WB_WAKE_ACTIVE   (1u << 1)
#define WB_WAKE_APPROVAL (1u << 2)
#define WB_WAKE_ASK      (1u << 3)

// 给 UI 层用的只读快照。UI 不应直接持有业务层指针。
typedef struct {
    bool connected;
    bool have_status;
    int64_t clock_s;
    int n_sess;
    wb_sess_t sess[WB_MAX_SESS];

    bool approval;
    int approval_id;
    char tool[WB_TOOL_LEN];
    char hint[WB_HINT_LEN];

    bool ask;
    int ask_id;
    int ask_ans;       // 0 = 只读
    int ask_multi;
    int n_opts;
    char text[WB_TEXT_LEN];
    char opt[WB_OPT_MAX][WB_OPT_LEN];

    wb_page_t page;
    int sel;           // 总览：会话下标；审批：0=允许 1=拒绝；提问：选项下标
    bool busy;         // 有 RUNNING 会话或待处理审批/提问
    int beep_vol;      // 主机 cfg.beep_vol，<=0 表示静音

    // 额度页数据：来自 credit 帧（{"remain":x,"total":y}）。
    bool have_credit;
    double credit_remain;
    double credit_total;
    int64_t credit_at; // 收到 credit 帧时的主机墙钟（epoch 秒），0 = 未知

    // 主机大盘：usage 帧（30s 一跳）与 host 帧。have_* 区分「没数据」和「0」。
    bool have_usage;
    double usage_today;
    double usage_cache;
    int usage_req;
    int usage_models;      // usage_model[] 里有效条数（已剔除无名项）
    wb_usage_model_t usage_model[WB_USAGE_MODELS];
    bool have_host;
    int gw_ports;
    int gw_sessions;
    int uptime_s;
    bool gw_secret;
    bool gw_cli;
} wb_view_t;

typedef void (*wb_biz_change_cb_t)(void);

// 启动 BLE 外设 + 收帧任务。只调用一次；失败返回 false（不阻塞启动流程）。
bool wb_biz_start(wb_biz_change_cb_t on_change);

// 线程安全快照。
void wb_biz_snapshot(wb_view_t *out);

// 取走自上次调用以来累积的唤醒原因位掩码。
unsigned wb_biz_take_wake(void);

void wb_biz_set_connected(bool connected);

// 三键业务动作（短按 UP/DOWN → move；短按 OK → confirm；长按 DOWN → back）。
void wb_biz_move(int delta);
void wb_biz_confirm(void);
void wb_biz_back(void);

// 主机 cfg.beep_test：取走一次性的“测试提示音”请求。
bool wb_biz_take_test_beep(void);

// 上行帧。
bool wb_biz_send_hello(void);
bool wb_biz_send_battery(int soc);

#ifdef __cplusplus
}
#endif
