// passport-fw/main/wb/wb_ui.cc —— WD Hub 看板：UI 适配层。
//
// 视觉全部交给 wb_view.c（wd-hub-design 方向 A「仪表像素」的实机实现，
// 从 passport-workbuddy 工程原样移植）。本文件只做三件事：
//   1. 把业务层快照 wb_view_t 适配成 wb_view.c 需要的 wb_state_t；
//   2. 把三键映射到「前 5 页 ↑↓ 轮转 + 审批/提问事件页 + 待机覆盖」；
//   3. 维持 wb_ui.h 的对外 API 不变，app_wb.cc 一行不用改。
//
// 本轮按约定「只移植已有实现」：主机端协议未提供的字段（最近事件、下行帧数、
// 审批倒计时）留占位，界面会显示「暂无数据」类文案，不伪造数字。
// 逐模型用量明细原先也在占位之列，现已打通（usage 帧 models[].n/v/hit →
// wb_proto → wb_biz → wb_state_t.usage.model[]），不在本层编造任何值。
// 「主机资源」页同样是纯搬运：host 帧的 cpu/mem/disk 字段经 wb_proto →
// wb_biz → wb_state_t.res，缺字段时不编造 0。
//
// 旧 V2 实现（1596 行、10 屏预创建 + 共享 chrome）备份在
// passport-fw/.tmp/ui-rewrite-20261105/wb_ui_legacy.cc。
#include "wb_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "wb_view.h"
#include "wb_link.h"
#include "wd_core.h"

namespace {

bool s_inited;
wb_state_t s_st;
int s_soc = -1;
int64_t s_boot_ms;

// 提问页交互软状态。
//   commit_* —— OK 确认后先让「已提交」在屏上停一帧以上再真正上行，
//               否则业务层会立刻把页切回总览，用户看不到确认结果。
//   edge_*   —— 撞到首/末项时页脚提示的到期时刻（到期后恢复默认文案）。
struct {
    int commit_id;          // 已确认的题号，0 = 没有
    int commit_idx;         // 已确认的选项下标
    int64_t commit_ms;
    bool drawn;             // 「已提交」态是否已渲染过至少一帧
    int64_t edge_until_ms;
} s_ask;

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

    // usage 帧：合计值 + 逐模型明细都要搬过去。明细以前被协议层丢掉了
    // （只留数组长度），屏幕上 5 行进度条因此永远是空的。
    s_st.usage.valid = v->have_usage;
    s_st.usage.today = (int64_t)v->usage_today;
    s_st.usage.req = v->usage_req;
    s_st.usage.cached = (int64_t)v->usage_cache;
    int n_model = v->usage_models;
    if (n_model > WB_USAGE_MODELS) n_model = WB_USAGE_MODELS;
    if (n_model < 0) n_model = 0;
    s_st.usage.n_model = n_model;
    for (int i = 0; i < n_model; ++i) {
        copy_str(s_st.usage.model[i].name, sizeof(s_st.usage.model[i].name),
                 v->usage_model[i].name);
        s_st.usage.model[i].tokens = v->usage_model[i].tokens;
        s_st.usage.model[i].cached = v->usage_model[i].cached;
    }

    s_st.beep_vol = v->beep_vol;
    s_st.skin = 1;
    s_st.soc = s_soc;
    s_st.boot_ms = s_boot_ms;

    // 第 5 页「主机资源」：has_res 与 res 一起搬，缺数据时视觉层自己写空态。
    s_st.has_res = v->has_res;
    s_st.res = v->res;

    // 关于页「网关」卡：host 帧的五个字段以前在业务层就断了（adapt 一个都
    // 没搬），屏上因此只能写死。这里全部搬过来，has_host 决定空态文案。
    s_st.has_host = v->have_host;
    s_st.gw_ports = v->gw_ports;
    s_st.gw_sessions = v->gw_sessions;
    s_st.uptime_s = v->uptime_s;
    s_st.gw_secret = v->gw_secret;
    s_st.gw_cli = v->gw_cli;

    // 链路诊断：设备侧自采计数。以前 wb_view_set_activity() 只在 init 里
    // 写死调一次 (0,"—",false)，s_frames 无人累加、s_low_power 无人设置，
    // 于是「0 帧 / 正常档」永远不变。现在每帧 sync 都从 wb_link 取真值，
    // 顺便让 s_st.link.connected 覆盖 adapt 上面按 v->connected 推出来的值
    // ——B 端与业务层同一份真相，避免两处各自判断。
    wb_link_get_stats(&s_st.link);
}

// 「已提交」态渲染完这一帧之后才上行：业务层 confirm 会立刻把页切回总览，
// 先画后发才能让人看见确认结果。要求至少历经一次 sync（drawn），避免
// 按键正好卡在两帧之间时「已提交」一闪而过；再停留 HOLD 毫秒，让这一态
// 真的可读（UI 重绘由 1s 的 lv_timer 驱动，停留太短人眼只会看到闪一下）。
constexpr int64_t kAskCommitHoldMs = 3500;

// 撞到首/末项时页脚提示的停留时长。UI 重绘是 1s 一拍，2s 以下容易被下一帧
// 直接盖掉；3s 既够人读完，也不会把常态提示挤掉太久。
constexpr int64_t kAskEdgeHintMs = 3000;

void ask_after_sync(void)
{
    if (s_ask.commit_id <= 0) return;
    if (!s_ask.drawn) {
        s_ask.drawn = true;
        return;
    }
    if (now_ms() - s_ask.commit_ms < kAskCommitHoldMs) return;
    wb_biz_confirm();                 // 上行 index，业务层清 ask 并回总览
    s_ask.commit_id = 0;
    s_ask.commit_idx = -1;
    s_ask.drawn = false;
    wb_view_set_ask_committed(0, -1);
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
    // 关于/链路页的链路统计与网关字段现在都有真实来源（wb_link_get_stats
    // 在每帧 adapt 里搬运，host 帧字段也从业务层搬过来了）。这里不再写死
    // 占位值 —— 「0 帧 / 正常档」以前就是这么进屏的。
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
    ask_after_sync();
}

bool wb_ui_key(wb_ui_key_t key, bool long_press)
{
    if (!s_inited) return false;

    const wb_view_page_t page = wb_view_current_page(&s_st);

    // 待机页：任意键只唤醒（PRESS 阶段 app_wb.cc 已续期），不产生业务动作。
    if (page == WB_VIEW_STANDBY) return true;

    // 审批页：OK 允许 / 长 DOWN 拒绝，交给业务层原有的 confirm / back。
    if (page == WB_VIEW_APPROVAL) return false;

    // 提问页：↑↓ 切换选项、OK 确认当前项、长 DOWN 跳过。
    // 业务层短 OK 本来就是 confirm，这里只在「确认前先显示已提交态」上做
    // 一层包装；游标仍由业务层 wb_biz_move 钳制，UI 只镜像到视觉层。
    if (page == WB_VIEW_ASK) {
        if (long_press) {
            // 事件页不抢长 OK：留给底座 2s 提示 / 5s 关机。
            // 「长按 OK 回首页」只在导航页生效，避免作答中被误带出页面。
            if (key == WB_UI_KEY_OK) return false;
            // 长 DOWN = 跳过（业务层既定语义）；已提交后不再重复上报。
            if (key == WB_UI_KEY_DOWN && s_ask.commit_id <= 0) wb_biz_back();
            return true;
        }

        wb_view_t view;
        wb_biz_snapshot(&view);
        const int n_opts = view.n_opts > WB_OPT_MAX ? WB_OPT_MAX : view.n_opts;

        // 已进入「已提交」态：锁住所有键，避免二次上报造成状态不一致。
        if (s_ask.commit_id > 0) return true;

        if (key == WB_UI_KEY_UP || key == WB_UI_KEY_DOWN) {
            if (n_opts <= 0) {
                s_ask.edge_until_ms = now_ms() + kAskEdgeHintMs;
                wb_view_set_ask_edge(0, s_ask.edge_until_ms);   // 仅提示无选项
                return true;
            }
            const int before = view.sel;
            wb_biz_move(key == WB_UI_KEY_UP ? -1 : 1);
            wb_biz_snapshot(&view);
            wb_view_set_ask_cursor(view.sel);
            if (view.sel == before) {            // 已经顶到首项 / 末项
                s_ask.edge_until_ms = now_ms() + kAskEdgeHintMs;
                wb_view_set_ask_edge(key == WB_UI_KEY_UP ? -1 : 1,
                                     s_ask.edge_until_ms);
            } else {
                s_ask.edge_until_ms = 0;
                wb_view_set_ask_edge(0, 0);
            }
            return true;
        }

        if (key == WB_UI_KEY_OK) {
            if (n_opts <= 0) {                   // 无选项：不提交，只提示
                s_ask.edge_until_ms = now_ms() + kAskEdgeHintMs;
                wb_view_set_ask_edge(0, s_ask.edge_until_ms);
                return true;
            }
            s_ask.commit_id = view.ask_id;
            s_ask.commit_idx = view.sel;
            s_ask.commit_ms = now_ms();
            s_ask.drawn = false;
            wb_view_set_ask_committed(s_ask.commit_id, s_ask.commit_idx);
            return true;
        }
        return true;
    }

    // 导航页（STATUS / RACK / USAGE / ABOUT / HOSTRES）：↑↓ 轮转，长按 OK 回首页。
    // 长按 UP/DOWN 仍不翻页（长 DOWN 由业务层当 back 处理）。
    // 页数由 wb_view 统一提供，这里不再写死常量。
    if (long_press) {
        if (key == WB_UI_KEY_OK) {
            // 800ms 的 LONG 事件与底座 2s 提示 / 5s 关机是两条独立链路：
            // main.cc 的注释明确写了「0.8s 长按已返回一层，之后继续按住才
            // 弹关机提示」，两者不冲突。hold_ok=true 也保证 registry 不会抢。
            wb_view_set_manual_page(WB_VIEW_STATUS);
            return true;
        }
        return false;
    }

    const int n = wb_view_nav_page_count();
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
        // 短 OK 从「回首页」腾出来后是保留空位（后续给「进入子页」用）。
        // 必须在这里吞掉：返回 false 会落到 app_wb.cc 的 wb_biz_confirm()，
        // 有待办时按一下短 OK 就会误提交审批 / 提问，是严重回归。
        return true;
    }
    return false;
}

bool wb_ui_tick(void)
{
    if (!s_inited) return false;

    // 待机页的时钟靠 now_ms 推进，需要每秒触发一次 sync；其余页由业务层
    // 的 dirty 标志驱动，不额外重绘。
    bool need = wb_view_current_page(&s_st) == WB_VIEW_STANDBY;

    // 「已提交」待上行：连续两帧，保证这一态至少被渲染一次（见 ask_after_sync）。
    if (s_ask.commit_id > 0) need = true;

    // 边界提示到期：清掉并请求一帧，把页脚恢复成默认文案。
    if (s_ask.edge_until_ms && now_ms() >= s_ask.edge_until_ms) {
        s_ask.edge_until_ms = 0;
        wb_view_set_ask_edge(0, 0);
        need = true;
    }
    return need;
}

}  // extern "C"
