// 待机页（大眼睛）状态机单测 —— 直接编译真实 wb_core.c，非 mock。
//
// 覆盖 6 条：
//   A 空闲满 3 分钟才进待机页（差 1 秒不进）
//   B 有 RUNNING 会话时不进待机页
//   C 有审批/提问时不进待机页（优先级高于待机）
//   D 离线不进待机页（断链时该显示"离线"，不能把故障藏进大眼睛）
//   E 待机页一收到下行帧立刻退出
//   F 待机页按键 → 回到配额总览
#include <stdio.h>
#include <string.h>

#include "wb_core.h"

static int fails;

static void ck(bool ok, const char *what)
{
    if (ok) printf("  OK  %s\n", what);
    else { printf("  FAIL %s\n", what); fails++; }
}

static const char *pg(wb_page_t p)
{
    switch (p) {
    case WB_PAGE_STATUS:   return "1总览";
    case WB_PAGE_RACK:     return "2列表";
    case WB_PAGE_RUN:      return "3运行";
    case WB_PAGE_APPROVAL: return "4审批";
    case WB_PAGE_ASK:      return "5提问";
    case WB_PAGE_STANDBY:  return "6待机";
    default: return "?";
    }
}

// 造一个"主机在正常推数据"的状态：有下行帧、无任务。
static void idle_state(wb_state_t *s, int64_t *now, wb_tick_t *t)
{
    wb_core_init(s, *now);
    s->sess[0].state[0] = 0;
    s->n_sess = 0;
}

// 模拟"每隔 2 秒收到一帧状态"（真实主机的节奏），推进到 3 分钟门槛附近。
// 为什么必须持续喂帧：空闲判据里有一条是"最近 WB_OFFLINE_MS 内收到过下行帧"，
// 不喂帧会先被判成离线，那样永远进不了待机页（这正是 D 要验的）。
static void feed_until(wb_state_t *s, int64_t *now, wb_tick_t *t, int64_t upto_ms)
{
    while (*now < upto_ms) {
        *now += 2000;
        s->last_rx_ms = *now;      // 主机推了一帧
        wb_core_tick(s, *now, t);
    }
}

int main(void)
{
    int64_t now = 1000;
    wb_tick_t t;

    puts("[A] 空闲满 3 分钟才进待机页（179s 不进，180s 进）");
    {
        wb_state_t s;
        idle_state(&s, &now, &t);
        // 先过一次开机抑制，再吃到"刚空闲 179 秒"
        feed_until(&s, &now, &t, now + 179000);
        printf("      179s -> %s\n", pg(wb_core_page(&s)));
        ck(wb_core_page(&s) != WB_PAGE_STANDBY, "179 秒还没进待机页");
        feed_until(&s, &now, &t, now + 3000);
        printf("      182s -> %s\n", pg(wb_core_page(&s)));
        ck(wb_core_page(&s) == WB_PAGE_STANDBY, "过 3 分钟进待机页");
    }

    puts("[B] 有 RUNNING 会话 → 不进待机页");
    {
        wb_state_t s;
        idle_state(&s, &now, &t);
        snprintf(s.sess[0].state, WB_STATE_LEN, "RUNNING");
        s.n_sess = 1;
        feed_until(&s, &now, &t, now + 300000);   // 5 分钟
        printf("      -> %s\n", pg(wb_core_page(&s)));
        ck(wb_core_page(&s) != WB_PAGE_STANDBY, "干活时不打瞌睡");
    }

    puts("[C] 有审批 → 待机页让位给审批页");
    {
        wb_state_t s;
        idle_state(&s, &now, &t);
        s.n_sess = 0;
        // 先让它进入待机页
        feed_until(&s, &now, &t, now + 200000);
        ck(wb_core_page(&s) == WB_PAGE_STANDBY, "先进入待机页");
        // 审批来了
        s.appr.active = true;
        s.appr.id = 7;
        snprintf(s.appr.tool, WB_TOOL_LEN, "bash");
        s.appr.deadline_ms = now + WB_APPROVAL_TTL_MS;
        s.last_rx_ms = now;
        wb_core_tick(&s, now, &t);
        printf("      -> %s\n", pg(wb_core_page(&s)));
        ck(wb_core_page(&s) == WB_PAGE_APPROVAL, "审批优先级高于待机页");
        ck(!s.standby_page, "待机页已被收回");
    }

    puts("[D] 离线 → 不进待机页（不能把断链藏起来）");
    {
        wb_state_t s;
        idle_state(&s, &now, &t);
        // 不喂帧，只推进时间：先判离线，再等 3 分钟
        for (int i = 0; i < 120; i++) { now += 1000; wb_core_tick(&s, now, &t); }
        printf("      offline=%d -> %s\n", (int)s.offline, pg(wb_core_page(&s)));
        ck(wb_core_page(&s) != WB_PAGE_STANDBY, "离线时不进待机页");
    }

    puts("[E] 待机页出现新任务 → 立刻退出");
    {
        wb_state_t s;
        idle_state(&s, &now, &t);
        feed_until(&s, &now, &t, now + 200000);
        ck(wb_core_page(&s) == WB_PAGE_STANDBY, "先进入待机页");
        // 主机推来一帧"有新任务在跑"的状态
        now += 2000;
        snprintf(s.sess[0].state, WB_STATE_LEN, "RUNNING");
        s.n_sess = 1;
        s.last_rx_ms = now;
        wb_core_tick(&s, now, &t);
        printf("      -> %s standby_changed=%d\n", pg(wb_core_page(&s)),
               (int)t.standby_changed);
        ck(wb_core_page(&s) != WB_PAGE_STANDBY, "来了新任务立刻醒");
        ck(t.standby_changed, "standby_changed 置位（调用方据此停动画）");
    }

    puts("[E2] 纯心跳帧（无任务变化）不该把待机页叫醒");
    {
        wb_state_t s;
        idle_state(&s, &now, &t);
        feed_until(&s, &now, &t, now + 200000);
        ck(wb_core_page(&s) == WB_PAGE_STANDBY, "先进入待机页");
        now += 2000;
        s.last_rx_ms = now;      // 只是心跳
        wb_core_tick(&s, now, &t);
        printf("      -> %s\n", pg(wb_core_page(&s)));
        ck(wb_core_page(&s) == WB_PAGE_STANDBY, "心跳不打断待机（否则永远睡不着）");
    }

    puts("[F] 待机页按键 → 回到配额总览");
    {
        wb_state_t s;
        idle_state(&s, &now, &t);
        feed_until(&s, &now, &t, now + 200000);
        ck(wb_core_page(&s) == WB_PAGE_STANDBY, "先进入待机页");
        wb_core_goto_page(&s, WB_PAGE_STATUS);
        printf("      -> %s\n", pg(wb_core_page(&s)));
        ck(wb_core_page(&s) == WB_PAGE_STATUS, "按键唤醒回总览");
        ck(!s.standby_page, "待机标志已清（不能压在上面导致按了没反应）");
        /* 只清 standby 标志还不够：控制台任务在同一轮紧接着调 wb_core_tick()，
         * 若空闲计时没一并清零，tick 会判定"仍然空闲超时"立刻把待机页压回去，
         * 唤醒边沿被吃掉 —— 真机表现就是"待机时按键没反应"。 */
        wb_core_tick(&s, now + 1, &t);
        ck(!s.standby_page, "唤醒后不会被下一轮 tick 压回待机页");
    }

    printf("\n失败数: %d\n", fails);
    return fails ? 1 : 0;
}
