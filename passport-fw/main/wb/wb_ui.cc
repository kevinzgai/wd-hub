// passport-fw/main/wb/wb_ui.cc —— 占位界面（不是最终视觉稿）。
//
// 设计稿定稿后只替换本文件即可：业务层（wb_biz）只提供只读快照，不依赖
// 任何坐标、字号或颜色。所有布局常量集中在本文件顶部。
//
// 信息结构（与 S3 看板一致）：
//   总览：连接状态 + 最多 3 个会话（名称/状态/模型/消息）
//   审批：工具名 + 提示 + 允许/拒绝
//   提问：问题 + 最多 6 个选项（或“请在电脑上回答”）
//   待机：大号时间 + 日期 + “无人打扰 · 在线” + 唤醒提示
#include "wb_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_fonts.h"
#include "ui_palette.h"
#include "ui_pixel.h"

LV_FONT_DECLARE(buddy_font_16);
LV_FONT_DECLARE(home_font_12);
LV_FONT_DECLARE(pomodoro_digits_48);

namespace {

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 320;
constexpr int MARGIN = 12;
constexpr int CONTENT_W = SCREEN_W - MARGIN * 2;

constexpr int OVERVIEW_ROW_Y = 86;
constexpr int OVERVIEW_ROW_H = 58;
constexpr int OVERVIEW_ROW_GAP = 62;

constexpr int FOOTER_Y = 293;

// 待机页刻意用纯黑 + 纯白：50% 背光下对比度最高，不依赖主题色。
constexpr uint32_t STANDBY_BG = 0x05070A;
constexpr uint32_t STANDBY_TEXT = 0xFFFFFF;
constexpr uint32_t STANDBY_SUB = 0xD8E1EA;
constexpr uint32_t STANDBY_ACCENT = 0x6FD3FF;

struct Row {
    lv_obj_t *panel;
    lv_obj_t *name;
    lv_obj_t *state;
    lv_obj_t *model;
};

struct UiState {
    lv_obj_t *scr_overview;
    lv_obj_t *scr_approval;
    lv_obj_t *scr_ask;
    lv_obj_t *scr_standby;

    lv_obj_t *ov_conn;
    lv_obj_t *ov_status;
    lv_obj_t *ov_empty;
    Row rows[WB_MAX_SESS];
    lv_obj_t *ov_footer;

    lv_obj_t *ap_tool;
    lv_obj_t *ap_hint;
    lv_obj_t *ap_allow;
    lv_obj_t *ap_deny;
    lv_obj_t *ap_allow_txt;
    lv_obj_t *ap_deny_txt;
    lv_obj_t *ap_footer;

    lv_obj_t *aq_text;
    lv_obj_t *aq_ro;
    lv_obj_t *aq_opt_panel[WB_OPT_MAX];
    lv_obj_t *aq_opt_txt[WB_OPT_MAX];
    lv_obj_t *aq_footer;

    lv_obj_t *sb_time;
    lv_obj_t *sb_date;
    lv_obj_t *sb_status;
    lv_obj_t *sb_hint;

    wb_page_t loaded_page;
    bool loaded_standby;
    bool loaded_any;
    bool inited;
};

UiState s_ui;

lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    ui_pixel_background(scr);
    return scr;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                     int x, int y, int w, int h, lv_text_align_t align)
{
    lv_obj_t *l = ui_pixel_label(parent, "", font, color);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

lv_obj_t *footer(lv_obj_t *parent, const char *text)
{
    // 提示行较长，占位版用小一号字避免被 LV_LABEL_LONG_DOT 截断。
    return make_label(parent, &home_font_12, UI_TEXT_DIM, MARGIN, FOOTER_Y + 2,
                      CONTENT_W, 18, LV_TEXT_ALIGN_CENTER);
}

void build_overview(void)
{
    s_ui.scr_overview = new_screen();
    ui_pixel_title_plate(s_ui.scr_overview, 6, "会话总览", &buddy_font_16);

    s_ui.ov_conn = make_label(s_ui.scr_overview, &home_font_12, UI_TEXT,
                              MARGIN, 40, CONTENT_W, 16, LV_TEXT_ALIGN_LEFT);
    s_ui.ov_status = make_label(s_ui.scr_overview, &home_font_12, UI_ACCENT,
                                MARGIN, 60, CONTENT_W, 16, LV_TEXT_ALIGN_LEFT);

    for (int i = 0; i < WB_MAX_SESS; ++i) {
        Row *r = &s_ui.rows[i];
        r->panel = ui_pixel_panel_create(s_ui.scr_overview, MARGIN,
                                          OVERVIEW_ROW_Y + i * OVERVIEW_ROW_GAP,
                                          CONTENT_W, OVERVIEW_ROW_H, UI_SURFACE);
        r->name = make_label(r->panel, &buddy_font_16, UI_TEXT, 8, 4, 150, 20,
                             LV_TEXT_ALIGN_LEFT);
        r->state = make_label(r->panel, &home_font_12, UI_ACCENT, 158, 6, 50, 16,
                              LV_TEXT_ALIGN_RIGHT);
        r->model = make_label(r->panel, &home_font_12, UI_TEXT_DIM, 8, 25, 200, 16,
                              LV_TEXT_ALIGN_LEFT);
    }

    s_ui.ov_empty = make_label(s_ui.scr_overview, &buddy_font_16, UI_TEXT_DIM,
                               MARGIN, 140, CONTENT_W, 24, LV_TEXT_ALIGN_CENTER);

    s_ui.ov_footer = footer(s_ui.scr_overview, "短按 ↑↓ 选择 · OK 查看 · 长按 DOWN 返回");
}

void build_approval(void)
{
    s_ui.scr_approval = new_screen();
    ui_pixel_title_plate(s_ui.scr_approval, 6, "需要审批", &buddy_font_16);

    s_ui.ap_tool = make_label(s_ui.scr_approval, &buddy_font_16, UI_ACCENT,
                              MARGIN, 40, CONTENT_W, 20, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *hint_panel = ui_pixel_panel_create(s_ui.scr_approval, MARGIN, 68,
                                                 CONTENT_W, 116, UI_SURFACE);
    s_ui.ap_hint = make_label(hint_panel, &buddy_font_16, UI_TEXT, 8, 6, 200, 104,
                              LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(s_ui.ap_hint, LV_LABEL_LONG_WRAP);

    const int opt_w = (CONTENT_W - 8) / 2;
    s_ui.ap_allow = ui_pixel_panel_create(s_ui.scr_approval, MARGIN, 200, opt_w, 34,
                                          UI_SURFACE);
    s_ui.ap_deny = ui_pixel_panel_create(s_ui.scr_approval, MARGIN + opt_w + 8, 200,
                                         opt_w, 34, UI_SURFACE);
    s_ui.ap_allow_txt = make_label(s_ui.ap_allow, &buddy_font_16, UI_TEXT, 0, 7,
                                   opt_w, 20, LV_TEXT_ALIGN_CENTER);
    s_ui.ap_deny_txt = make_label(s_ui.ap_deny, &buddy_font_16, UI_TEXT, 0, 7,
                                  opt_w, 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ui.ap_allow_txt, "允许");
    lv_label_set_text(s_ui.ap_deny_txt, "拒绝");

    s_ui.ap_footer = footer(s_ui.scr_approval, "↑↓ 选择 · OK 确认 · 长按 DOWN 返回");
}

void build_ask(void)
{
    s_ui.scr_ask = new_screen();
    ui_pixel_title_plate(s_ui.scr_ask, 6, "需要回答", &buddy_font_16);

    lv_obj_t *text_panel = ui_pixel_panel_create(s_ui.scr_ask, MARGIN, 38,
                                                 CONTENT_W, 76, UI_SURFACE);
    s_ui.aq_text = make_label(text_panel, &buddy_font_16, UI_TEXT, 8, 4, 200, 68,
                              LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(s_ui.aq_text, LV_LABEL_LONG_WRAP);

    s_ui.aq_ro = make_label(s_ui.scr_ask, &buddy_font_16, UI_TEXT_DIM,
                            MARGIN, 120, CONTENT_W, 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ui.aq_ro, "请在电脑上回答");

    for (int i = 0; i < WB_OPT_MAX; ++i) {
        s_ui.aq_opt_panel[i] = ui_pixel_panel_create(
            s_ui.scr_ask, MARGIN, 122 + i * 27, CONTENT_W, 25, UI_SURFACE);
        s_ui.aq_opt_txt[i] = make_label(s_ui.aq_opt_panel[i], &buddy_font_16,
                                        UI_TEXT, 6, 3, CONTENT_W - 12, 19,
                                        LV_TEXT_ALIGN_LEFT);
        lv_obj_add_flag(s_ui.aq_opt_panel[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_ui.aq_footer = footer(s_ui.scr_ask, "↑↓ 选择 · OK 回答 · 长按 DOWN 跳过");
}

void build_standby(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(STANDBY_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_ui.scr_standby = scr;

    s_ui.sb_time = make_label(scr, &pomodoro_digits_48, STANDBY_TEXT,
                              0, 96, SCREEN_W, 60, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ui.sb_time, "--:--");

    s_ui.sb_date = make_label(scr, &buddy_font_16, STANDBY_SUB,
                              0, 164, SCREEN_W, 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ui.sb_date, "---- -- --");

    s_ui.sb_status = make_label(scr, &buddy_font_16, STANDBY_ACCENT,
                                0, 194, SCREEN_W, 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ui.sb_status, "无人打扰 · 在线");

    s_ui.sb_hint = make_label(scr, &buddy_font_16, STANDBY_SUB,
                              0, 286, SCREEN_W, 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ui.sb_hint, "按任意键唤醒");
}

void update_standby(const wb_view_t *v)
{
    char time_buf[8] = "--:--";
    char date_buf[40] = "---- -- --";

    if (v->clock_s > 0) {
        time_t t = (time_t)v->clock_s;
        struct tm tm_local;
        gmtime_r(&t, &tm_local);   // t 已是主机本地墙钟，不能再套本地时区
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
        static const char *k_week[] = {"周日", "周一", "周二", "周三",
                                       "周四", "周五", "周六"};
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d %s",
                 tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                 k_week[tm_local.tm_wday % 7]);
    }

    ui_pixel_label_set_text(s_ui.sb_time, time_buf);
    ui_pixel_label_set_text(s_ui.sb_date, date_buf);
    ui_pixel_label_set_text(s_ui.sb_status,
                            v->connected ? "无人打扰 · 在线" : "等待主机连接");
}

const char *sess_name(const wb_view_t *v, int i)
{
    return v->sess[i].name[0] ? v->sess[i].name : "未命名会话";
}

void update_overview(const wb_view_t *v)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s",
             v->connected ? "已连接主机 · 在线" : "等待主机连接…");
    ui_pixel_label_set_text(s_ui.ov_conn, buf);

    if (v->have_status)
        snprintf(buf, sizeof(buf), "%s · %d 个会话", v->busy ? "运行中" : "空闲", v->n_sess);
    else
        snprintf(buf, sizeof(buf), "尚未收到主机状态");
    ui_pixel_label_set_text(s_ui.ov_status, buf);

    lv_obj_add_flag(s_ui.ov_empty, LV_OBJ_FLAG_HIDDEN);
    if (v->n_sess == 0) {
        lv_obj_remove_flag(s_ui.ov_empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ui.ov_empty, v->connected ? "暂无会话 · 无人打扰" : "等待连接");
    }

    for (int i = 0; i < WB_MAX_SESS; ++i) {
        Row *r = &s_ui.rows[i];
        if (i >= v->n_sess) {
            lv_obj_add_flag(r->panel, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(r->panel, LV_OBJ_FLAG_HIDDEN);
        ui_pixel_label_set_text(r->name, sess_name(v, i));
        ui_pixel_label_set_text(r->state, v->sess[i].state);

        snprintf(buf, sizeof(buf), "%s%s%s", v->sess[i].model,
                 v->sess[i].model[0] && v->sess[i].msg[0] ? " · " : "",
                 v->sess[i].msg);
        ui_pixel_label_set_text(r->model, buf);
        ui_pixel_set_selected(r->panel, i == v->sel, true);
    }
}

void update_approval(const wb_view_t *v)
{
    ui_pixel_label_set_text(s_ui.ap_tool, v->tool[0] ? v->tool : "WorkBuddy");
    ui_pixel_label_set_text(s_ui.ap_hint, v->hint[0] ? v->hint : "主机请求执行操作");

    ui_pixel_set_selected(s_ui.ap_allow, v->sel == 0, true);
    ui_pixel_set_selected(s_ui.ap_deny, v->sel == 1, true);
    ui_pixel_text_color(s_ui.ap_allow_txt, v->sel == 0 ? UI_ON_ACCENT : UI_TEXT);
    ui_pixel_text_color(s_ui.ap_deny_txt, v->sel == 1 ? UI_ON_ACCENT : UI_TEXT);
}

void update_ask(const wb_view_t *v)
{
    ui_pixel_label_set_text(s_ui.aq_text, v->text[0] ? v->text : "主机需要你的回答");

    const bool read_only = v->ask_ans == 0 || v->n_opts == 0;
    if (read_only) {
        lv_obj_remove_flag(s_ui.aq_ro, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.aq_ro, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < WB_OPT_MAX; ++i) {
        if (i >= v->n_opts || read_only) {
            lv_obj_add_flag(s_ui.aq_opt_panel[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ui.aq_opt_panel[i], LV_OBJ_FLAG_HIDDEN);
        ui_pixel_label_set_text(s_ui.aq_opt_txt[i], v->opt[i]);
        ui_pixel_set_selected(s_ui.aq_opt_panel[i], i == v->sel, true);
        ui_pixel_text_color(s_ui.aq_opt_txt[i], i == v->sel ? UI_ON_ACCENT : UI_TEXT);
    }
}

}  // namespace

void wb_ui_init(void)
{
    if (s_ui.inited) return;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.loaded_page = WB_PAGE_OVERVIEW;
    s_ui.loaded_any = false;
    build_overview();
    build_approval();
    build_ask();
    build_standby();
    s_ui.inited = true;
}

void wb_ui_deinit(void)
{
    if (!s_ui.inited) return;
    lv_obj_t *screens[] = {s_ui.scr_overview, s_ui.scr_approval, s_ui.scr_ask, s_ui.scr_standby};
    for (lv_obj_t *s : screens)
        if (s) lv_obj_delete(s);
    memset(&s_ui, 0, sizeof(s_ui));
}

void wb_ui_sync(const wb_view_t *v, bool standby)
{
    if (!s_ui.inited || !v) return;

    if (standby) {
        update_standby(v);
        if (!s_ui.loaded_standby) {
            lv_screen_load(s_ui.scr_standby);
            s_ui.loaded_standby = true;
            s_ui.loaded_any = true;
        }
        return;
    }

    if (s_ui.loaded_standby) {
        s_ui.loaded_standby = false;
        s_ui.loaded_any = false;   // 强制重新加载业务页
    }

    if (!s_ui.loaded_any || s_ui.loaded_page != v->page) {
        switch (v->page) {
        case WB_PAGE_APPROVAL: lv_screen_load(s_ui.scr_approval); break;
        case WB_PAGE_ASK: lv_screen_load(s_ui.scr_ask); break;
        default: lv_screen_load(s_ui.scr_overview); break;
        }
        s_ui.loaded_page = v->page;
        s_ui.loaded_any = true;
    }

    switch (v->page) {
    case WB_PAGE_APPROVAL: update_approval(v); break;
    case WB_PAGE_ASK: update_ask(v); break;
    default: update_overview(v); break;
    }
}
