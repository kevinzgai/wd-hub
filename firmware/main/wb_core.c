// main/wb_core.c —— wb_core.h 的实现。
#include "wb_core.h"

#include <string.h>

void wb_core_init(wb_state_t *st, int64_t now_ms)
{
    memset(st, 0, sizeof(*st));
    st->boot_ms = now_ms;
    st->last_rx_ms = now_ms;
    st->run_idle_since = -1;
    /* 必须显式置 -1：memset 后它是 0，而空闲判据是 `idle_since_ms < 0`
     * 表示"不空闲"。留 0 的话第一次 tick 就会算成 `now - 0` = 巨大，
     * 直接判成"闲了很久"、开机第一帧就跳到待机页（实测踩到过）。 */
    st->idle_since_ms = -1;
    st->soc = -1;
    st->beep_vol = 100;          // 与主机 BEEP_VOL_DEFAULT 对齐,连上后以 cfg 覆盖
    st->credit.level = 0;
    for (int i = 0; i < WB_BEEP_KIND_COUNT; i++) st->last_beep_ms[i] = -100000;
}

uint8_t wb_credit_level(double remain, double total, uint8_t prev_level)
{
    if (total <= 0.0) return 0;
    double r = remain / total;
    if (r < 0.0) r = 0.0;

    switch (prev_level) {
    case 2:
        // 已在红档:要升过 13% 才褪红,再按 20% 判是否留下橙档。
        if (r > 0.13) return (r > 0.20) ? 0 : 1;
        return 2;
    case 1:
        if (r > 0.23) return 0;
        if (r <= 0.10) return 2;
        return 1;
    default:
        if (r <= 0.10) return 2;
        if (r <= 0.20) return 1;
        return 0;
    }
}

int wb_core_running_count(const wb_state_t *st)
{
    int n = 0;
    for (int i = 0; i < st->n_sess; i++) {
        if (strcmp(st->sess[i].state, "RUNNING") == 0) n++;
    }
    return n;
}

int wb_core_hidden_sessions(const wb_state_t *st)
{
    if (st->count > st->n_sess) return st->count - st->n_sess;
    return 0;
}

static void maybe_beep(wb_state_t *st, wb_beep_kind_t kind, int64_t now_ms, wb_tick_t *out)
{
    if (st->beep_vol <= 0) return;   // 静音档:不出声,但事件本身仍然记录
    if (now_ms - st->last_beep_ms[kind] < WB_BEEP_DEDUP_MS) return;
    st->last_beep_ms[kind] = now_ms;
    out->beep = true;
    out->beep_kind = kind;
}

bool wb_core_apply(wb_state_t *st, const wb_ev_t *ev, int64_t now_ms, wb_tick_t *out)
{
    wb_tick_t sink;
    wb_tick_t *side = out ? out : &sink;
    memset(side, 0, sizeof(*side));
    bool redraw = false;

    switch (ev->type) {
    case WB_EV_STATUS: {
        char prev_state[WB_MAX_SESS][WB_STATE_LEN];
        const int prev_n = st->n_sess;
        for (int i = 0; i < prev_n; i++) {
            memcpy(prev_state[i], st->sess[i].state, WB_STATE_LEN);
        }

        const int n = ev->n_sess < WB_MAX_SESS ? ev->n_sess : WB_MAX_SESS;
        for (int i = 0; i < n; i++) st->sess[i] = ev->sess[i];
        st->n_sess = n;
        st->count = ev->count;

        // 任务跑完:同一序号上从 RUNNING 变成 COMPLETED/IDLE 即提示一声。
        for (int i = 0; i < n && i < prev_n; i++) {
            const bool was_running = strcmp(prev_state[i], "RUNNING") == 0;
            const bool running_now = strcmp(st->sess[i].state, "RUNNING") == 0;
            if (was_running && !running_now) {
                maybe_beep(st, WB_BEEP_DONE, now_ms, side);
                break;
            }
        }
        redraw = true;
        break;
    }
    case WB_EV_CREDIT:
        st->credit.remain = ev->remain;
        st->credit.total = ev->total;
        st->credit.valid = true;
        st->credit.level = wb_credit_level(ev->remain, ev->total, st->credit.level);
        redraw = true;
        break;

    case WB_EV_APPROVAL:
        // 审批页强制切页,优先级最高;同 id 重发只刷新内容与计时,不重置任何本地状态。
        if (st->appr.active && st->appr.id == ev->approval_id) {
            memcpy(st->appr.tool, ev->tool, sizeof(st->appr.tool));
            memcpy(st->appr.hint, ev->hint, sizeof(st->appr.hint));
            st->appr.deadline_ms = now_ms + WB_APPROVAL_TTL_MS;
        } else {
            memset(&st->appr, 0, sizeof(st->appr));
            st->appr.active = true;
            st->appr.id = ev->approval_id;
            memcpy(st->appr.tool, ev->tool, sizeof(st->appr.tool));
            memcpy(st->appr.hint, ev->hint, sizeof(st->appr.hint));
            st->appr.deadline_ms = now_ms + WB_APPROVAL_TTL_MS;
            maybe_beep(st, WB_BEEP_APPROVAL, now_ms, side);
        }
        redraw = true;
        break;

    case WB_EV_ASK:
        // 60s 保活会重发同一个 ask:必须幂等——只更新文案、重置计时器,
        // 不重建页面、不重置用户已经移动的高亮位置。
        //
        // ⚠ 多选降级：本固件**没有实现**多选勾选（单键板只有短按/长按，
        // 没有第三种手势去勾选，wb_view.c 也不画 [ ]/[x] 勾选框）。
        // 若照收 multi=1，用户会停在一个死页面上：短按只移动光标，
        // mask 恒为 0，submit_ask 以"多选未勾选"拒绝提交 —— 怎么按都没反应。
        // 所以这里按 WB_CAPS（本固件实际实现的能力位）归零：宁可语义退化，
        // 也不能把用户卡死。主机侧也用 CAP_ASK_MULTI 做了同样的协商
        // （双保险：两边任一降级都能避免死页面）。
        {
            // 真实现了多选勾选时，WB_CAPS 会带上 WB_CAP_ASK_MULTI，
            // 这里自然放行，无需改代码。
            const int multi = (WB_CAPS & WB_CAP_ASK_MULTI) ? ev->ask_multi : 0;
            if (st->ask.active && st->ask.id == ev->ask_id) {
                memcpy(st->ask.text, ev->text, sizeof(st->ask.text));
                st->ask.ans = ev->ask_ans;
                st->ask.multi = multi;
                st->ask.deadline_ms = now_ms + WB_ASK_TTL_MS;
            } else {
                memset(&st->ask, 0, sizeof(st->ask));
                st->ask.active = true;
                st->ask.id = ev->ask_id;
                st->ask.ans = ev->ask_ans;
                st->ask.multi = multi;
                memcpy(st->ask.tool, ev->tool, sizeof(st->ask.tool));
                memcpy(st->ask.text, ev->text, sizeof(st->ask.text));
                st->ask.n_opts = ev->n_opts;
                for (int i = 0; i < ev->n_opts && i < WB_OPT_MAX; i++) {
                    memcpy(st->ask.opt[i], ev->opt[i], WB_OPT_LEN);
                }
                st->ask.deadline_ms = now_ms + WB_ASK_TTL_MS;
                maybe_beep(st, WB_BEEP_ASK, now_ms, side);
            }
        }
        redraw = true;
        break;

    case WB_EV_ASK_END:
        if (st->ask.active && (ev->ask_id == 0 || st->ask.id == ev->ask_id)) {
            memset(&st->ask, 0, sizeof(st->ask));
            redraw = true;
        }
        break;

    case WB_EV_CFG:
        st->beep_vol = ev->beep_vol;
        redraw = true;
        break;

    case WB_EV_USAGE:
        /* 整帧覆盖，不做增量累加（见 wb_core.h 里 usage 字段的注释）。
         * 只在数字真的变了才 redraw：30s 一跳的帧里大部分时候
         * today 是不动的，每帧都置 redraw 会让屏幕每 30 秒闪一次。 */
        if (!st->usage.valid || st->usage.today != ev->usage.today ||
            st->usage.req != ev->usage.req ||
            st->usage.n_model != ev->usage.n_model) {
            redraw = true;
        }
        st->usage = ev->usage;
        break;

    default:
        break;
    }

    st->linked = true;
    st->last_rx_ms = now_ms;
    st->offline = false;

    return redraw;
}

void wb_core_tick(wb_state_t *st, int64_t now_ms, wb_tick_t *out)
{
    memset(out, 0, sizeof(*out));

    // 1) 离线:6s 没有下行帧。设备自己判,不等主机告知——实测主机要 40s+ 才发现。
    const bool offline = (now_ms - st->last_rx_ms) > WB_OFFLINE_MS;
    if (offline != st->offline) {
        st->offline = offline;
        out->offline_changed = true;
        out->redraw = true;
    }

    // 2) 审批超时:主机 90s fail-open,设备 95s 自动退出,避免僵尸页面。
    if (st->appr.active && now_ms >= st->appr.deadline_ms) {
        memset(&st->appr, 0, sizeof(st->appr));
        out->approval_expired = true;
        out->page_changed = true;
        out->redraw = true;
    }

    // 3) 提问超时:主机每 60s 保活重推,90s 没等到就说明主机那边已经处理掉了。
    if (st->ask.active && now_ms >= st->ask.deadline_ms) {
        memset(&st->ask, 0, sizeof(st->ask));
        out->ask_expired = true;
        out->page_changed = true;
        out->redraw = true;
    }

    // 4) 自动切页:首次出现 RUNNING 即进运行页;全部离开后停留 3s 再退回。
    const int running = wb_core_running_count(st);
    if (running > 0) {
        st->run_idle_since = -1;
        if (!st->run_seen) {
            st->run_seen = true;
            st->run_start_ms = now_ms;
            st->auto_suppressed = false;   // 新一轮运行重新给自动切页一次机会
        } else if (st->run_start_ms == 0) {
            st->run_start_ms = now_ms;
        }
        if (!st->run_page && !st->auto_suppressed && !st->run_manual &&
            (now_ms - st->boot_ms) >= WB_RUN_BOOT_GUARD_MS &&
            !st->appr.active && !st->ask.active) {
            st->run_page = true;
            st->run_manual = false;   // 自动切来的，允许按"任务结束"收回
            out->page_changed = true;
            out->redraw = true;
        }
    } else {
        if (st->run_idle_since < 0) st->run_idle_since = now_ms;
        // 全部离开 RUNNING 且过了停留窗口:收回运行页并把本轮计时清零。
        // 注意这里不能只在 run_page 为真时才清——用户可能早就手动离开了页面,
        // 若不无条件清,下一轮任务的"已运行时长"会从上一次的起点算起。
        if ((now_ms - st->run_idle_since) >= WB_RUN_EXIT_MS) {
            /* 只在"自动切过来的"页面收回。
             *
             * 用户自己短按翻到第 3 页时（run_manual=true）不收回：
             * 三页循环是纯手动导航，用户翻到哪页就该停在哪页。
             * 若这里也收回，没有任务在跑的时候短按到第 3 页会闪一下就跳走。 */
            if (st->run_page && !st->run_manual) {
                st->run_page = false;
                out->page_changed = true;
                out->redraw = true;
            }
            if (st->run_seen) {
                st->run_seen = false;
                st->run_start_ms = 0;
            }
            st->auto_suppressed = false;
        }
    }

    /* 5) 空闲 → 待机页（大眼睛）。
     *
     * "空闲"的判据，任一不成立就清零重来：
     *   · 有审批 / 提问 —— 主机在等人处理，绝不能打瞌睡
     *   · 有 RUNNING 会话 —— 正在干活的屏本来就该露着
     *   · 离线 —— 断链时屏幕该停在那页显示"离线"，切去大眼睛反而把
     *     故障藏起来了（和原看板"离线立刻退出休眠"是同一条原则）
     *
     * ⚠ **不能把"收到下行帧"算进空闲判据**（这里踩过一次）：主机每 2 秒
     * 推一帧 status_list，那是心跳不是"有人在干活"。若把它当活动信号，
     * last_rx_ms 永远新鲜、busy 恒真，设备一辈子进不了待机页 —— 实测
     * test_standby.c 用例 A 就是因此卡在 179s/182s 都停在总览页。
     *
     * 三分钟门槛沿用原看板休眠屏（见 wb_core.h 的 WB_IDLE_TO_STANDBY_MS）。
     * 进/出都用 standby_changed 告知调用方，由它启停眼睛动画
     * （动画耗电，不能一直算帧）。 */
#ifdef WB_TEST_IGNORE_BUSY
    /* 测试钩子（与 ui_board 的同名宏一致）：忽略"有任务/有审批/离线"，
     * 让设备在有人干活时也能进入待机页，用来实测眼睛动画。
     * 真实的 3 分钟 + 必须空闲两个条件叠加时没法验收。 */
    const bool busy = false;
#else
    const bool busy = st->appr.active || st->ask.active || (running > 0) || st->offline;
#endif
    if (busy) {
        st->idle_since_ms = -1;
        if (st->standby_page) {
            // 一有动静立刻收回，回到原来的导航页。
            st->standby_page = false;
            out->standby_changed = true;
            out->page_changed = true;
            out->redraw = true;
        }
    } else if (!st->standby_page) {
        if (st->idle_since_ms < 0) {
            st->idle_since_ms = now_ms;      // 刚开始空闲
        } else if ((now_ms - st->idle_since_ms) >= WB_IDLE_TO_STANDBY_MS) {
            st->standby_page = true;
            out->standby_changed = true;
            out->page_changed = true;
            out->redraw = true;
        }
    }
}

wb_page_t wb_core_page(const wb_state_t *st)
{
    if (st->appr.active) return WB_PAGE_APPROVAL;
    if (st->ask.active) return WB_PAGE_ASK;
    if (st->standby_page) return WB_PAGE_STANDBY;
    if (st->run_page) return WB_PAGE_RUN;
    if (st->usage_page) return WB_PAGE_USAGE;
    return st->rack_page ? WB_PAGE_RACK : WB_PAGE_STATUS;
}

void wb_core_goto_page(wb_state_t *st, wb_page_t page)
{
    // 审批/提问不是导航页：翻过去只能看到空页。
    if (page != WB_PAGE_RACK && page != WB_PAGE_RUN && page != WB_PAGE_STANDBY &&
        page != WB_PAGE_USAGE) {
        page = WB_PAGE_STATUS;
    }

    st->rack_page    = (page == WB_PAGE_RACK);
    st->run_page     = (page == WB_PAGE_RUN);
    st->run_manual   = (page == WB_PAGE_RUN);   // 手动来的，任务结束也不收回
    st->usage_page   = (page == WB_PAGE_USAGE);
    /* 短按翻页会把用户从待机页叫醒。不这么做的话：自动切到待机页后
     * 短按只是在改 rack/run 标志，而 standby_page 还压在上面，
     * wb_core_page() 继续返回 STANDBY —— 表现就是"按了没反应"。 */
    st->standby_page = false;

    /* 压住自动切页。三页里只有运行页是"自动切页的目标"，
     * 翻到总览/会话列表时若不压，tick 下一轮（≤100ms）就会因为
     * running>0 把用户弹回运行页 —— 表现就是短按翻页完全没反应。
     *
     * 翻到运行页本身也要压：否则用户从运行页翻回总览的那一刻，
     * auto_suppressed 已被 tick 清掉，又会立刻弹回来。
     * 下一次"新一轮运行"（run_seen 归零后重新见到 RUNNING）会自动放开。 */
    st->auto_suppressed = true;
}

int64_t wb_core_run_elapsed_s(const wb_state_t *st, int64_t now_ms)
{
    if (st->run_start_ms <= 0) return 0;
    const int64_t d = (now_ms - st->run_start_ms) / 1000;
    return d > 0 ? d : 0;
}
