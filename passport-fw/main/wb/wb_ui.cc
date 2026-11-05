// passport-fw/main/wb/wb_ui.cc —— WD Hub 看板：UI 适配层。
//
// 视觉全部交给 wb_view.c（wd-hub-design 方向 A「仪表像素」的实机实现，
// 从 passport-workbuddy 工程原样移植）。本文件只做三件事：
//   1. 把业务层快照 wb_view_t 适配成 wb_view.c 需要的 wb_state_t；
//   2. 把三键映射到「前 4 页 ↑↓ 轮转 + 审批/提问事件页 + 待机覆盖」；
//   3. 维持 wb_ui.h 的对外 API 不变，app_wb.cc 一行不用改。
//
// 本轮按约定「只移植已有实现」：主机端协议未提供的字段（逐模型用量明细、
// 最近事件、下行帧数、审批倒计时）留占位，界面会显示「暂无数据」类文案，
// 不伪造数字。协议扩展、离线/低电覆盖页留待后续。
//
// 旧 V2 实现（1596 行、10 屏预创建 + 共享 chrome）备份在
// passport-fw/.tmp/ui-rewrite-20261105/wb_ui_legacy.cc。
#include "wb_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "wb_view.h"
#include "wd_core.h"

namespace {

bool s_inited;
wb_state_t s_st;
int s_soc = -1;
int64_t s_boot_ms;

int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

// 额度分级只影响大数字/进度条颜色：≤10% 红、≤20% 橙、其余主色。
uint8_t credit_level(double remain, double total)
{
    if (total <= 0.0) return 0;
    const double pct = remain * 100.0 / total;
    if (pct <= 10.0) return 2;
    if (pct <= 20.0) return 1;
    return 0;
}

void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void adapt(const wb_view_t *v, bool standby)
{
    memset(&s_st, 0, sizeof(s_st));

    s_st.credit.valid = v->have_credit;
    s_st.credit.remain = v->credit_remain;
    s_st.credit.total = v->credit_total;
    s_st.credit.level = credit_level(v->credit_remain, v->credit_total);

    s_st.n_sess = v->n_sess > WB_MAX_SESS ? WB_MAX_SESS : v->n_sess;
    s_st.count = s_st.n_sess;
    for (int i = 0; i < s_st.n_sess; ++i) {
        memcpy(&s_st.sess[i], &v->sess[i], sizeof(wb_sess_t));
    }

    s_st.appr.active = v->approval;
    s_st.appr.id = v->approval_id;
    copy_str(s_st.appr.tool, sizeof(s_st.appr.tool), v->tool);
    copy_str(s_st.appr.hint, sizeof(s_st.appr.hint), v->hint);

    s_st.ask.active = v->ask;
    s_st.ask.id = v->ask_id;
    s_st.ask.ans = v->ask_ans;
    s_st.ask.multi = v->ask_multi;
    s_st.ask.n_opts = v->n_opts > WB_OPT_MAX ? WB_OPT_MAX : v->n_opts;
    s_st.ask.cursor = v->sel;
    copy_str(s_st.ask.text, sizeof(s_st.ask.text), v->text);
    for (int i = 0; i < s_st.ask.n_opts; ++i) {
        copy_str(s_st.ask.opt[i], sizeof(s_st.ask.opt[i]), v->opt[i]);
    }

    // 本工程只有 BLE 连接态，没有「收到过帧」的独立标志；直接映射即可，
    // 断连时页头徽标会显示红色「离线」。
    s_st.linked = v->connected;
    s_st.offline = !v->connected;

    s_st.standby_page = standby;
    s_st.clock_local_s = v->clock_s;
    s_st.clock_sync_ms = now_ms();

    // usage 帧只有合计值，没有逐模型明细：n_model=0 让今日消耗页显示
    // 「今日暂无模型数据」，不编造 5 行进度条。
    s_st.usage.valid = v->have_usage;
    s_st.usage.today = (int64_t)v->usage_today;
    s_st.usage.req = v->usage_req;
    s_st.usage.cached = (int64_t)v->usage_cache;
    s_st.usage.n_model = 0;

    s_st.beep_vol = v->beep_vol;
    s_st.skin = 1;
    s_st.soc = s_soc;
    s_st.boot_ms = s_boot_ms;
}

}  // namespace

extern "C" {

void wb_ui_init(void)
{
    if (s_inited) return;
    s_boot_ms = now_ms();
    s_soc = -1;
    wb_view_build();
    wb_view_set_manual_page(WB_VIEW_STATUS);
    // 关于/链路页的活动统计当前没有数据来源，先给占位，后续接协议。
    wb_view_set_activity(0, "—", false);
    s_inited = true;
}

void wb_ui_deinit(void)
{
    if (!s_inited) return;
    wb_view_teardown();
    s_inited = false;
}

void wb_ui_set_battery(int soc)
{
    s_soc = soc;
}

void wb_ui_sync(const wb_view_t *view, bool standby)
{
    if (!s_inited || !view) return;
    adapt(view, standby);
    wb_view_refresh(&s_st, now_ms());
}

bool wb_ui_key(wb_ui_key_t key, bool long_press)
{
    if (!s_inited) return false;

    const wb_view_page_t page = wb_view_current_page(&s_st);

    // 待机页：任意键只唤醒（PRESS 阶段 app_wb.cc 已续期），不产生业务动作。
    if (page == WB_VIEW_STANDBY) return true;

    // 审批页：OK 允许 / 长 DOWN 拒绝，交给业务层原有的 confirm / back。
    if (page == WB_VIEW_APPROVAL) return false;

    // 提问页：OK 下一项，长 DOWN 提交。业务层短 OK 是「提交」、长 DOWN 是
    // 「skip」，与设计稿页脚相反，所以这里由 UI 层消费并显式调用。
    if (page == WB_VIEW_ASK) {
        if (key == WB_UI_KEY_OK && long_press) return false;  // 长 OK 留给底座关机
        if (long_press) {
            if (key == WB_UI_KEY_DOWN) wb_biz_confirm();
            return true;
        }
        if (key == WB_UI_KEY_OK || key == WB_UI_KEY_DOWN || key == WB_UI_KEY_UP) {
            const int delta = (key == WB_UI_KEY_UP) ? -1 : 1;
            wb_biz_move(delta);
            wb_view_t snap;
            wb_biz_snapshot(&snap);
            wb_view_set_ask_cursor(snap.sel);
            return true;
        }
        return true;
    }

    // 前 4 个导航页：↑↓ 轮转，OK 回总览。长按不翻页。
    if (long_press) return false;

    const int n = WB_VIEW_ABOUT + 1;   // STATUS / RACK / USAGE / ABOUT
    const int cur = (int)wb_view_manual_page();
    if (key == WB_UI_KEY_UP) {
        wb_view_set_manual_page((wb_view_page_t)((cur + n - 1) % n));
        return true;
    }
    if (key == WB_UI_KEY_DOWN) {
        wb_view_set_manual_page((wb_view_page_t)((cur + 1) % n));
        return true;
    }
    if (key == WB_UI_KEY_OK) {
        wb_view_set_manual_page(WB_VIEW_STATUS);
        return true;
    }
    return false;
}

bool wb_ui_tick(void)
{
    if (!s_inited) return false;
    // 待机页的时钟靠 now_ms 推进，需要每秒触发一次 sync；其余页由业务层
    // 的 dirty 标志驱动，不额外重绘。
    return wb_view_current_page(&s_st) == WB_VIEW_STANDBY;
}

}  // extern "C"
