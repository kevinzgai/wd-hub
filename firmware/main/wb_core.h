// main/wb_core.h —— 控制台的状态与规则(纯逻辑,不碰 BLE / LVGL / NVS)。
//
// 这一层承载需求 v1.1 里所有"需要判定"的规则:积分分级(带回差)、会话三态、
// 离线判定、审批/提问超时、自动切页与防抢、事件提示音去重。把它做成纯逻辑的
// 唯一目的是:这些规则能用 host 单测逐条验(799ms/801ms、19%/21% 这种边界
// 在真机上验一次要几分钟),设备只负责画出来。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wb_proto.h"

// 本工程的其余代码是 C++;这些是纯 C 模块,需要 C 链接。
#ifdef __cplusplus
extern "C" {
#endif

// 离线判定:连续 3 个状态周期(2s)没有任何下行帧。
#define WB_OFFLINE_MS       6000
// 审批页自动退出:主机 90s 超时,留 5s 余量避免僵尸页面。
#define WB_APPROVAL_TTL_MS  95000
// 提问页自动退出:主机 60s 保活重推,设备 90s 无操作退出(与主机假设一致)。
#define WB_ASK_TTL_MS       90000
// 全部会话离开 RUNNING 后停留多久再退回状态页,避免抖动。
#define WB_RUN_EXIT_MS      3000
// 上电后的抑制窗口:避免开机瞬间就被自动切走。
#define WB_RUN_BOOT_GUARD_MS 5000
// 同一类事件提示音的最小间隔。
#define WB_BEEP_DEDUP_MS    3000
/* 空闲多久自动切到待机页（大眼睛）。
 *
 * 3 分钟沿用原看板休眠屏的门槛（ui_board 的 SLEEP_IDLE_MS）：服务端
 * working 计数带 30 分钟活跃窗口，两个任务之间正常的思考间隙会被判成
 * 空闲，时间太短会让屏幕反复睡/醒。3 分钟能盖住绝大多数思考间隙，
 * 又不会让人离开后白亮太久。
 *
 * 注意：这里的"空闲"是**本地判定**——只看有没有审批/提问、有没有
 * RUNNING 会话、是否离线。**不看有没有收到下行帧**：主机每 2 秒推一帧
 * status_list 是心跳，不是"有人在干活"，把它算成活动会导致永远睡不着。 */
#ifdef WB_TEST_STANDBY_FAST
/* 测试钩子：cmake 传 -DWB_TEST_STANDBY_FAST=1 把门槛降到 20 秒，
 * 用来实测待机页/眼睛动画（等真实的 3 分钟没法验收）。
 * 正式固件不带这个参数，行为不变。和 WB_TEST_IGNORE_BUSY 是同一套做法。 */
#define WB_IDLE_TO_STANDBY_MS (20 * 1000)
#else
#define WB_IDLE_TO_STANDBY_MS (3 * 60 * 1000)
#endif

typedef enum {
    WB_PAGE_STATUS = 0,   // TELEMETRY:配额仪表
    WB_PAGE_RACK,         // SESSION RACK:会话卡列表
    WB_PAGE_RUN,
    WB_PAGE_USAGE,        // 今日消耗:总量 + 请求数 + 按模型分布
    WB_PAGE_APPROVAL,
    WB_PAGE_ASK,
    WB_PAGE_STANDBY,      // 待机页:大眼睛 + 空闲时长（空闲自动切,按键即醒）
} wb_page_t;

typedef enum {
    WB_BEEP_ASK = 0,
    WB_BEEP_APPROVAL,
    WB_BEEP_DONE,
    WB_BEEP_KIND_COUNT,
} wb_beep_kind_t;

typedef struct {
    bool valid;        // 从未收到过积分帧时为 false,界面显示「—」而不是 0
    double remain;
    double total;
    uint8_t level;     // 0 主色 / 1 橙 / 2 红
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
    int ans;           // 0 = 只读
    int multi;         // 1 = 多选
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
    int count;         // 主机上报的会话总数语义值

    wb_approval_t appr;
    wb_ask_t ask;

    bool linked;       // 收到过任何下行帧
    bool offline;
    int64_t last_rx_ms;

    bool rack_page;          // 用户在会话列表页(与配额页互为上下页)
    bool run_page;           // 当前停在运行页
    bool usage_page;         // 当前停在今日消耗页（第 4 个导航页）
    /* run_page 有两种来源，必须分开记：
     *   false = 自动切页切过来的 —— 任务全部结束后 3 秒自动收回
     *   true  = 用户自己短按翻过来的 —— **不许自动收回**
     *
     * 不加这个标志的后果：短按翻到第 3 页时若当前没有 RUNNING 会话
     * （或刚跑完），tick 会在 3 秒内把 run_page 清掉、屏幕弹回第 1 页，
     * 表现为"翻到第三页只闪了一下就跳走"，三页循环直接断掉。 */
    bool run_manual;
    bool auto_suppressed;    // 用户手动离开后,本轮运行期不再自动抢
    bool run_seen;           // 本轮是否已经见过 RUNNING
    int64_t run_start_ms;    // 首次见到 RUNNING 的时刻(算已运行时长)
    int64_t run_idle_since;  // 无 RUNNING 会话的起始时刻,-1 表示当前有运行中

    /* 待机页（大眼睛）。**只能由空闲计时自动进入**，不并入短按翻页环
     * （用户确认的方案是"空闲自动切"）—— 所以不需要区分手动/自动来源。
     *
     * 空闲计时用 idle_since_ms：有审批/提问、有 RUNNING 会话、或最近
     * 收到过下行帧，三者任一成立就把计时清零重来。这比"看服务端 working"
     * 可靠，也符合"没活干满 3 分钟才打瞌睡"的字面语义。
     *
     * 注意**离线不算空闲**：断链时屏幕该停在那页显示"离线"，
     * 切去大眼睛反而把故障藏起来了。 */
    bool standby_page;       // 当前停在待机页
    int64_t idle_since_ms;   // 空闲起点；<0 表示"现在不空闲"

    /* 今日 token 消耗（第 4 个导航页）。
     *
     * 数据来自主机的 usage 帧（30s 一跳，不是 status 的 2s）——
     * 因为它要扫一遍会话转录 jsonl，比读数据库重得多。
     *
     * 这里存的是**快照**不是累计：主机每次重算今日总量并整帧覆盖，
     * 设备不做任何跨帧累加。理由：
     *   1. 跨天时主机口径切到新的一天，设备若累加就会把昨天的带进来。
     *   2. 设备断线期间的用量主机不会补发，累加会永久少一块。
     *   3. 重算 = 幂等，丢帧/重复帧都不会让数字跑偏。 */
    wb_usage_t usage;

    int beep_vol;            // 0..100,主机 cfg 帧同步
    uint8_t skin;            // 0 赛博 HUD / 1 像素风
    int soc;                 // -1 = 读不到

    int64_t boot_ms;
    int64_t last_beep_ms[WB_BEEP_KIND_COUNT];
} wb_state_t;

typedef struct {
    bool redraw;
    bool beep;
    wb_beep_kind_t beep_kind;
    bool page_changed;
    bool offline_changed;
    bool approval_expired;
    bool ask_expired;
    bool standby_changed;    // 进/出待机页（调用方据此启停眼睛动画）
} wb_tick_t;

void wb_core_init(wb_state_t *st, int64_t now_ms);

// 积分分级。返回 0/1/2,带 3% 回差(≤20% 橙、≤10% 红;>23% 褪橙、>13% 褪红)。
// 纯函数,单独暴露就是为了方便单测边界。
uint8_t wb_credit_level(double remain, double total, uint8_t prev_level);

// 应用一帧(已解码)。返回 true 表示界面需要重绘;out(可为 NULL)带出本帧
// 触发的提示音等副作用。
bool wb_core_apply(wb_state_t *st, const wb_ev_t *ev, int64_t now_ms, wb_tick_t *out);

// 周期性驱动:离线判定、超时退出、自动切页。返回本周期发生的事件。
void wb_core_tick(wb_state_t *st, int64_t now_ms, wb_tick_t *out);

// 当前应该显示哪一页。审批页优先级最高(阻塞且有时限),其次提问页。
wb_page_t wb_core_page(const wb_state_t *st);

int wb_core_running_count(const wb_state_t *st);

/* 用一次手势直接把界面翻到某个导航页（短按翻页用）。
 *
 * 只接受三个导航页（STATUS / RACK / RUN）—— 审批页、提问页
 * 不是"能翻到的页面"，由主机事件驱动，硬翻过去只会看到一张空白页。
 * 传别的值按 STATUS 处理。
 *
 * 会顺手把自动切页压住，并给运行页打上"手动进入"标记（任务跑完也不收回）。 */
void wb_core_goto_page(wb_state_t *st, wb_page_t page);

// 运行计时(秒)。只在见过 RUNNING 时有意义。
int64_t wb_core_run_elapsed_s(const wb_state_t *st, int64_t now_ms);

// 「还有 N 个」:主机发的是已发条数时 count<=n_sess,此时返回 0 不显示。
int wb_core_hidden_sessions(const wb_state_t *st);

#ifdef __cplusplus
}  // extern "C"
#endif
