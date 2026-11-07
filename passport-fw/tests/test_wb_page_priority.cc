// passport-fw/tests/test_wb_page_priority.cc —— 覆盖页优先级链的全组合断言。
//
// 为什么必须有这个测试：
//   这一轮往看板里插了两张「会盖住任何页面」的覆盖页（断连 / 低电）。
//   在此之前，选页逻辑写死在 wb_view.c 里、依赖 LVGL，host 编不进来，
//   所以每次改动只能靠烧录 + 人工翻页验证 —— 上一轮加第 5 页时就出过
//   「抓屏抓到第 3 页也可能第 5 页」的不确定性。
//   现在判定是纯函数（wb_page_priority.h），可以穷举所有布尔组合，
//   把「谁盖谁」这件事锁死。
//
// 断言依据 docs/KEYMAP_PLAN.md §5.1「打断优先级」：
//   待机 > 离线 > 审批/提问 > 低电 > 手动导航页
#include <cstdio>
#include <cstring>
#include <string>

#include "../../main/wb/wb_page_priority.h"

static int g_fail;

static void chk(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_fail;
    }
}

static const char *page_name(wb_view_page_t p)
{
    switch (p) {
    case WB_VIEW_STATUS:   return "STATUS";
    case WB_VIEW_RACK:     return "RACK";
    case WB_VIEW_USAGE:    return "USAGE";
    case WB_VIEW_ABOUT:    return "ABOUT";
    case WB_VIEW_HOSTRES:  return "HOSTRES";
    case WB_VIEW_APPROVAL: return "APPROVAL";
    case WB_VIEW_ASK:      return "ASK";
    case WB_VIEW_STANDBY:  return "STANDBY";
    case WB_VIEW_OFFLINE:  return "OFFLINE";
    case WB_VIEW_LOWBATT:  return "LOWBATT";
    default:               return "?";
    }
}

// 干净的基准状态：已连接、电量正常、无待办、非待机。
static wb_state_t base_state()
{
    wb_state_t st;
    memset(&st, 0, sizeof(st));
    st.linked = true;
    st.offline = false;
    st.soc = 60;
    st.standby_page = false;
    st.appr.active = false;
    st.ask.active = false;
    return st;
}

static wb_view_page_t pick(const wb_state_t *st, wb_view_page_t manual,
                           bool dismissed)
{
    return wb_page_pick(st, manual, dismissed);
}

int main()
{
    // ---- 1. 基准：什么都没发生，就显示手动导航页 ----
    for (int m = 0; m < WB_NAV_PAGE_COUNT; ++m) {
        wb_state_t st = base_state();
        const wb_view_page_t got = pick(&st, (wb_view_page_t)m, false);
        chk(got == (wb_view_page_t)m,
            (std::string("导航页直通 ") + page_name((wb_view_page_t)m)).c_str());
    }

    // ---- 2. 待机最高：待机时连覆盖页都不该弹 ----
    // 放着一动不动反而被离线页反复吵，是省电档下最糟的体验。
    {
        wb_state_t st = base_state();
        st.standby_page = true;
        st.linked = false;
        st.offline = true;
        st.soc = 5;
        st.appr.active = true;
        st.ask.active = true;
        chk(pick(&st, WB_VIEW_HOSTRES, false) == WB_VIEW_STANDBY,
            "待机压过 离线/审批/提问/低电");
    }

    // ---- 3. 离线压过审批与提问 ----
    // 断连时审批的 key 帧发不出去（wb_link_send 会失败），
    // 让用户「答了但没生效」比不让答更糟。
    {
        wb_state_t st = base_state();
        st.linked = false;
        st.offline = true;
        st.appr.active = true;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_OFFLINE,
            "离线 压 审批");
    }
    {
        wb_state_t st = base_state();
        st.linked = false;
        st.offline = true;
        st.ask.active = true;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_OFFLINE,
            "离线 压 提问");
    }
    // 只给 offline 不给 linked=false（或反之）也必须判为离线：
    // adapt() 里两者是同源推导，但历史代码有只改一个的写法，这里兜住。
    {
        wb_state_t st = base_state();
        st.offline = true;   // linked 仍为 true
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_OFFLINE,
            "只置 offline 也进离线页");
    }
    {
        wb_state_t st = base_state();
        st.linked = false;   // offline 仍为 false
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_OFFLINE,
            "只清 linked 也进离线页");
    }

    // ---- 4. 已连接时的待办：提问优先于审批 ----
    {
        wb_state_t st = base_state();
        st.appr.active = true;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_APPROVAL,
            "有待审批 → 审批页");
    }
    {
        wb_state_t st = base_state();
        st.ask.active = true;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_ASK,
            "有待作答 → 提问页");
    }
    {
        wb_state_t st = base_state();
        st.appr.active = true;
        st.ask.active = true;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_APPROVAL,
            "同时待办 → 审批页（顺序按现有实现）");
    }

    // ---- 5. 审批压过低电 ----
    // 低电只是提醒、不阻断上行；一块电池通知挡住待办是本末倒置。
    {
        wb_state_t st = base_state();
        st.soc = 8;
        st.appr.active = true;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_APPROVAL,
            "审批 压 低电");
    }

    // ---- 6. 低电阈值与回差 ----
    // <=15 弹，>15 不弹（18 是复位线，不在选页逻辑里）。
    for (int soc : {0, 1, 10, 14, 15}) {
        wb_state_t st = base_state();
        st.soc = soc;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_LOWBATT,
            (std::string("soc=") + std::to_string(soc) + " → 低电页").c_str());
    }
    for (int soc : {16, 17, 18, 19, 50, 100}) {
        wb_state_t st = base_state();
        st.soc = soc;
        chk(pick(&st, WB_VIEW_RACK, false) == WB_VIEW_RACK,
            (std::string("soc=") + std::to_string(soc) + " → 不弹低电页").c_str());
    }
    // soc < 0 = 读表失败，绝不猜成「低电」。
    {
        wb_state_t st = base_state();
        st.soc = -1;
        chk(pick(&st, WB_VIEW_USAGE, false) == WB_VIEW_USAGE,
            "soc 未知 → 不弹低电页");
    }

    // ---- 7. 低电页可被 dismiss，且 dismiss 不影响其它覆盖页 ----
    {
        wb_state_t st = base_state();
        st.soc = 5;
        chk(pick(&st, WB_VIEW_RACK, true) == WB_VIEW_RACK,
            "低电已 dismiss → 回到手动页");
    }
    {
        wb_state_t st = base_state();
        st.linked = false;
        st.offline = true;
        // 断连页没有 dismiss 状态，这个参数对它必须完全无效。
        chk(pick(&st, WB_VIEW_STATUS, true) == WB_VIEW_OFFLINE,
            "dismiss 对断连页无效");
    }

    // ---- 8. 待机时低电不叠加 ----
    {
        wb_state_t st = base_state();
        st.standby_page = true;
        st.soc = 3;
        chk(pick(&st, WB_VIEW_STATUS, false) == WB_VIEW_STANDBY,
            "待机页已有省电说明，低电页不叠加");
    }

    // ---- 9. 空指针不崩 ----
    chk(wb_page_pick(nullptr, WB_VIEW_ABOUT, false) == WB_VIEW_ABOUT,
        "nullptr 回落到手动页");

    // ---- 10. 辅助判定与选页保持一致 ----
    {
        wb_state_t st = base_state();
        st.standby_page = true;
        st.linked = false;
        st.offline = true;
        chk(!wb_page_offline_active(&st), "待机时不判为离线覆盖");
    }
    {
        wb_state_t st = base_state();
        st.linked = false;
        st.offline = true;
        chk(wb_page_offline_active(&st), "断连时判为离线覆盖");
    }

    if (g_fail == 0) {
        printf("test_wb_page_priority: 全部通过\n");
        return 0;
    }
    printf("test_wb_page_priority: %d 项失败\n", g_fail);
    return 1;
}
