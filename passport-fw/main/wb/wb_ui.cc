// passport-fw/main/wb/wb_ui.cc —— V2「大字流」视觉层。
//
// 设计稿：docs/ui-design/index.html 的 V2 列。
//   · 纯黑底 + 薄荷色单轨焦点；一屏一条会话。
//   · 28px 状态主字，底部 3 段进度轨表示“第几条 / 共几条”。
//   · 六屏：会话总览 / 审批 / 提问 / 待机 / 离线 / 低电量。
//   · 每屏底部保留三键键位栏。
//
// 本文件只做“快照 → 像素”。业务状态机、协议、待机/背光都在其它层：
// 这里不写任何 wb_biz 逻辑，也不改 wb_power 的行为。
// 唯一的 UI 级状态是“离线/低电覆盖页被用户按掉”，只影响本层显示。
#include "wb_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_fonts.h"
#include "esp_log.h"
#include "ui_pixel.h"

LV_FONT_DECLARE(wb_font_12);
LV_FONT_DECLARE(buddy_font_16);
LV_FONT_DECLARE(wb_font_28);

namespace {

const char *TAG = "wb_ui";

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 320;
constexpr int SAFE_L = 12;
constexpr int SAFE_R = 12;
constexpr int CONTENT_W = SCREEN_W - SAFE_L - SAFE_R;   // 216
constexpr int HEADER_Y = 10;
constexpr int HEADER_H = 28;
constexpr int FOOTER_Y = 274;
constexpr int FOOTER_H = 34;
constexpr int KEY_W[3] = {82, 58, 72};
constexpr int KEY_GAP = 2;
constexpr int FOCUS_X = 32;                             // 12 + 6px 轨 + 14 padding
constexpr int LOW_BATTERY_SOC = 15;                     // 低于该值显示低电覆盖页

// V2 调色板（与设计稿一一对应）。
constexpr uint32_t C_BG = 0x000000;
constexpr uint32_t C_PANEL = 0x0E0E0E;
constexpr uint32_t C_LINE = 0x2A2A2A;
constexpr uint32_t C_INK = 0xFFFFFF;
constexpr uint32_t C_DIM = 0xA8A8A8;
constexpr uint32_t C_MINT = 0x4AE3B5;
constexpr uint32_t C_GREEN = 0x7BE495;
constexpr uint32_t C_ORANGE = 0xFFB020;
constexpr uint32_t C_RED = 0xFF7070;
constexpr uint32_t C_SEG_IDLE = 0x4A4A4A;

enum Screen {
    SCR_OVERVIEW = 0,
    SCR_APPROVAL,
    SCR_ASK,
    SCR_STANDBY,
    SCR_OFFLINE,
    SCR_LOW,
    SCR_COUNT,
};

const char *const k_screen_name[SCR_COUNT] = {
    "overview", "approval", "ask", "standby", "offline", "low_battery",
};

// 顶栏状态簇：连接点 + 电量图形 + 电量百分比（每屏常驻）。
struct BatUi {
    lv_obj_t *dot;
    lv_obj_t *shell;
    lv_obj_t *fill;
    lv_obj_t *pct;
};

struct UiState {
    lv_obj_t *scr[SCR_COUNT];
    BatUi bat[SCR_COUNT];

    // 总览：一条会话 + 3 段进度轨（与 WB_MAX_SESS 对齐）。
    lv_obj_t *ov_idx;
    lv_obj_t *ov_tag;
    lv_obj_t *ov_kick;
    lv_obj_t *ov_state;
    lv_obj_t *ov_name;
    lv_obj_t *ov_model;
    lv_obj_t *ov_seg[WB_MAX_SESS];
    lv_obj_t *ov_next;

    // 审批。
    lv_obj_t *ap_idx;
    lv_obj_t *ap_tag;
    lv_obj_t *ap_bigq;
    lv_obj_t *ap_tool;
    lv_obj_t *ap_meta;
    lv_obj_t *ap_cmd;
    lv_obj_t *ap_act[2];
    lv_obj_t *ap_act_txt[2];

    // 提问。
    lv_obj_t *aq_idx;
    lv_obj_t *aq_tag;
    lv_obj_t *aq_kick;
    lv_obj_t *aq_text;
    lv_obj_t *aq_opt[WB_OPT_MAX];
    lv_obj_t *aq_num[WB_OPT_MAX];
    lv_obj_t *aq_opt_txt[WB_OPT_MAX];
    lv_obj_t *aq_ro;
    lv_obj_t *aq_hint;

    // 待机。
    lv_obj_t *sb_dot;
    lv_obj_t *sb_top;
    lv_obj_t *sb_clock;
    lv_obj_t *sb_date;
    lv_obj_t *sb_note;
    lv_obj_t *sb_hint;
    lv_obj_t *sb_sub;

    // 离线。
    lv_obj_t *of_idx;
    lv_obj_t *of_tag;
    lv_obj_t *of_title;
    lv_obj_t *of_sub;
    lv_obj_t *of_note;

    // 低电量。
    lv_obj_t *lb_idx;
    lv_obj_t *lb_tag;
    lv_obj_t *lb_fill;
    lv_obj_t *lb_pct;
    lv_obj_t *lb_title;
    lv_obj_t *lb_note;

    // 三键键位栏：0..2 = UP / OK / DOWN。
    lv_obj_t *key_kk[SCR_COUNT][3];
    lv_obj_t *key_ka[SCR_COUNT][3];

    int screen;            // Screen；-1 = 还没加载过
    bool inited;

    bool offline_dismissed;
    bool low_dismissed;
    int soc;               // <0 = 尚未读到

    int last_sel;
    int last_n;
    char last_token[WB_STATE_LEN];
    bool last_token_set;
};

UiState s_ui;

// ---------------- 基础构件 ----------------

lv_obj_t *screen_new(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

lv_obj_t *frame(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = frame(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

void left_rule(lv_obj_t *obj, uint32_t color, int width)
{
    lv_obj_set_style_border_width(obj, width, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
}

void all_rule(lv_obj_t *obj, uint32_t color, int width, int radius)
{
    lv_obj_set_style_border_width(obj, width, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_radius(obj, radius, 0);
}

lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y,
               int w, int h, lv_text_align_t align, int letter_space = 0)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    if (letter_space) lv_obj_set_style_text_letter_space(l, letter_space, 0);
    return l;
}

void set_text(lv_obj_t *label, const char *value)
{
    ui_pixel_label_set_text(label, value);
}

void set_color(lv_obj_t *obj, uint32_t color)
{
    if (obj) ui_pixel_bg_color(obj, color);
}

// 顶栏右侧状态簇：连接点 + 电池框 + 电量百分比。
void build_status_cluster(lv_obj_t *scr, int scr_idx, bool with_dot)
{
    BatUi &b = s_ui.bat[scr_idx];
    if (with_dot) b.dot = panel(scr, 178, 14, 6, 6, C_SEG_IDLE, 3);
    b.shell = panel(scr, 184, 13, 14, 8, C_BG, 2);
    all_rule(b.shell, C_LINE, 1, 2);
    b.fill = panel(scr, 185, 14, 1, 6, C_SEG_IDLE, 1);
    panel(scr, 198, 15, 2, 4, C_LINE, 1);   // 电池正极
    b.pct = text(scr, &wb_font_12, C_DIM, 201, 11, 27, 15, LV_TEXT_ALIGN_RIGHT);
    set_text(b.pct, "--%");
}

void update_status_cluster(int scr_idx, bool connected, int soc)
{
    BatUi &b = s_ui.bat[scr_idx];
    if (b.dot) set_color(b.dot, connected ? C_MINT : C_SEG_IDLE);
    if (b.pct) {
        if (soc < 0) {
            set_text(b.pct, "--%");
            lv_obj_set_width(b.fill, 1);
            set_color(b.fill, C_SEG_IDLE);
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", soc);
            set_text(b.pct, buf);
            int w = 10 * (soc > 100 ? 100 : soc) / 100;
            if (w < 1) w = 1;
            lv_obj_set_width(b.fill, w);
            set_color(b.fill, soc <= LOW_BATTERY_SOC ? C_RED
                                  : (soc <= 30 ? C_ORANGE : C_GREEN));
        }
    }
}

void build_header(lv_obj_t *scr, int scr_idx, lv_obj_t **left, lv_obj_t **right,
                  const char *l, const char *r)
{
    *left = text(scr, &wb_font_12, C_DIM, SAFE_L, HEADER_Y + 1, 100, 15,
                 LV_TEXT_ALIGN_LEFT, 2);
    *right = text(scr, &wb_font_12, C_DIM, 112, HEADER_Y + 1, 62, 15,
                  LV_TEXT_ALIGN_RIGHT);
    set_text(*left, l);
    set_text(*right, r);
    build_status_cluster(scr, scr_idx, true);
}

// 三键键位栏：左轨 2px，上排动作键名（12px 灰），下排当前动作（12px 白）。
void build_footer(lv_obj_t *scr, int scr_idx)
{
    int x = SAFE_L;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *cell = frame(scr, x, FOOTER_Y, KEY_W[i], FOOTER_H);
        left_rule(cell, C_LINE, 2);
        s_ui.key_kk[scr_idx][i] = text(cell, &wb_font_12, C_DIM, 7, 2,
                                       KEY_W[i] - 9, 15, LV_TEXT_ALIGN_LEFT);
        s_ui.key_ka[scr_idx][i] = text(cell, &wb_font_12, C_INK, 7, 18,
                                       KEY_W[i] - 9, 15, LV_TEXT_ALIGN_LEFT);
        x += KEY_W[i] + KEY_GAP;
    }
}

void set_keys(int scr_idx, const char *k0, const char *a0, const char *k1, const char *a1,
              const char *k2, const char *a2)
{
    set_text(s_ui.key_kk[scr_idx][0], k0);
    set_text(s_ui.key_ka[scr_idx][0], a0);
    set_text(s_ui.key_kk[scr_idx][1], k1);
    set_text(s_ui.key_ka[scr_idx][1], a1);
    set_text(s_ui.key_kk[scr_idx][2], k2);
    set_text(s_ui.key_ka[scr_idx][2], a2);
}

// ---------------- 六屏搭建 ----------------

void build_overview(void)
{
    lv_obj_t *scr = screen_new();
    s_ui.scr[SCR_OVERVIEW] = scr;
    build_header(scr, SCR_OVERVIEW, &s_ui.ov_idx, &s_ui.ov_tag, "00 / 00", "会话");

    // 6px 薄荷竖轨：整屏卡片化的唯一焦点语言。
    panel(scr, SAFE_L, 44, 6, 194, C_MINT, 0);

    s_ui.ov_kick = text(scr, &wb_font_12, C_DIM, FOCUS_X, 48, 196, 15,
                        LV_TEXT_ALIGN_LEFT, 1);
    s_ui.ov_state = text(scr, &wb_font_28, C_MINT, FOCUS_X, 66, 196, 34,
                         LV_TEXT_ALIGN_LEFT);
    panel(scr, FOCUS_X, 106, 36, 4, C_MINT, 2);
    s_ui.ov_name = text(scr, &buddy_font_16, C_INK, FOCUS_X, 118, 196, 42,
                        LV_TEXT_ALIGN_LEFT);
    s_ui.ov_model = text(scr, &wb_font_12, C_DIM, FOCUS_X, 164, 196, 15,
                         LV_TEXT_ALIGN_LEFT);

    // 3 段进度轨居中：3×22 + 2×6 = 78，起点 (240-78)/2 = 81。
    for (int i = 0; i < WB_MAX_SESS; ++i)
        s_ui.ov_seg[i] = panel(scr, 81 + i * 28, 242, 22, 4, C_SEG_IDLE, 2);

    s_ui.ov_next = text(scr, &wb_font_12, C_DIM, SAFE_L, 251, CONTENT_W, 15,
                        LV_TEXT_ALIGN_CENTER);

    build_footer(scr, SCR_OVERVIEW);
}

void build_approval(void)
{
    lv_obj_t *scr = screen_new();
    s_ui.scr[SCR_APPROVAL] = scr;
    build_header(scr, SCR_APPROVAL, &s_ui.ap_idx, &s_ui.ap_tag, "审批", "待处理");

    s_ui.ap_bigq = text(scr, &wb_font_28, C_RED, SAFE_L, 46, CONTENT_W, 34,
                        LV_TEXT_ALIGN_LEFT);
    set_text(s_ui.ap_bigq, "允许执行？");

    s_ui.ap_tool = text(scr, &buddy_font_16, C_INK, SAFE_L, 84, 150, 20,
                        LV_TEXT_ALIGN_LEFT);
    s_ui.ap_meta = text(scr, &wb_font_12, C_DIM, 162, 86, 66, 15,
                        LV_TEXT_ALIGN_RIGHT);
    set_text(s_ui.ap_meta, "等待确认");

    lv_obj_t *cmd = panel(scr, SAFE_L, 108, CONTENT_W, 88, C_PANEL, 0);
    left_rule(cmd, C_RED, 4);
    lv_obj_set_style_pad_all(cmd, 8, 0);
    s_ui.ap_cmd = text(cmd, &wb_font_12, C_DIM, 0, 0, 196, 72, LV_TEXT_ALIGN_LEFT);

    const int act_y[2] = {202, 240};
    for (int i = 0; i < 2; ++i) {
        s_ui.ap_act[i] = frame(scr, SAFE_L, act_y[i], CONTENT_W, 32);
        s_ui.ap_act_txt[i] = text(s_ui.ap_act[i], &buddy_font_16, C_DIM, 12, 6, 192, 20,
                                  LV_TEXT_ALIGN_LEFT);
    }
    set_text(s_ui.ap_act_txt[0], "允许执行 · OK 确认");
    set_text(s_ui.ap_act_txt[1], "直接拒绝 · 长按 DOWN");

    build_footer(scr, SCR_APPROVAL);
}

void build_ask(void)
{
    lv_obj_t *scr = screen_new();
    s_ui.scr[SCR_ASK] = scr;
    build_header(scr, SCR_ASK, &s_ui.aq_idx, &s_ui.aq_tag, "提问", "1 / 1");

    s_ui.aq_kick = text(scr, &wb_font_12, C_DIM, SAFE_L, 46, CONTENT_W, 15,
                        LV_TEXT_ALIGN_LEFT, 1);
    s_ui.aq_text = text(scr, &buddy_font_16, C_INK, SAFE_L, 62, CONTENT_W, 40,
                        LV_TEXT_ALIGN_LEFT);

    for (int i = 0; i < WB_OPT_MAX; ++i) {
        lv_obj_t *opt = frame(scr, SAFE_L, 112 + i * 25, CONTENT_W, 23);
        left_rule(opt, C_LINE, 6);
        s_ui.aq_opt[i] = opt;
        s_ui.aq_num[i] = text(opt, &wb_font_12, C_DIM, 2, 4, 18, 15, LV_TEXT_ALIGN_LEFT);
        s_ui.aq_opt_txt[i] = text(opt, &buddy_font_16, C_DIM, 24, 1, 182, 20,
                                  LV_TEXT_ALIGN_LEFT);
    }

    s_ui.aq_ro = text(scr, &buddy_font_16, C_DIM, SAFE_L, 150, CONTENT_W, 20,
                      LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.aq_ro, "请在电脑上回答");
    lv_obj_add_flag(s_ui.aq_ro, LV_OBJ_FLAG_HIDDEN);

    s_ui.aq_hint = text(scr, &wb_font_12, C_DIM, SAFE_L, 250, CONTENT_W, 15,
                        LV_TEXT_ALIGN_CENTER);

    build_footer(scr, SCR_ASK);
}

void build_standby(void)
{
    lv_obj_t *scr = screen_new();
    s_ui.scr[SCR_STANDBY] = scr;

    s_ui.sb_dot = panel(scr, SAFE_L, 17, 6, 6, C_MINT, 3);
    s_ui.sb_top = text(scr, &wb_font_12, C_DIM, SAFE_L + 12, 10, 150, 15,
                       LV_TEXT_ALIGN_LEFT);
    set_text(s_ui.sb_top, "无人打扰 · 在线");
    build_status_cluster(scr, SCR_STANDBY, false);

    panel(scr, 98, 120, 44, 4, C_MINT, 2);

    s_ui.sb_clock = text(scr, &wb_font_28, C_INK, 0, 132, SCREEN_W, 36,
                         LV_TEXT_ALIGN_CENTER, 4);
    set_text(s_ui.sb_clock, "--:--");

    s_ui.sb_date = text(scr, &buddy_font_16, C_MINT, 0, 174, SCREEN_W, 20,
                        LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.sb_date, "--月--日");

    s_ui.sb_note = text(scr, &buddy_font_16, C_DIM, 0, 198, SCREEN_W, 20,
                        LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.sb_note, "屏幕已休息");

    s_ui.sb_hint = text(scr, &buddy_font_16, C_INK, 0, 238, SCREEN_W, 20,
                        LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.sb_hint, "按任意键唤醒");

    s_ui.sb_sub = text(scr, &wb_font_12, C_DIM, 0, 258, SCREEN_W, 15,
                       LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.sb_sub, "长按 OK 2s 提示 · 5s 关机");

    build_footer(scr, SCR_STANDBY);
    set_keys(SCR_STANDBY, "UP", "唤醒", "OK", "唤醒", "长按OK", "关机");
}

void build_offline(void)
{
    lv_obj_t *scr = screen_new();
    s_ui.scr[SCR_OFFLINE] = scr;
    build_header(scr, SCR_OFFLINE, &s_ui.of_idx, &s_ui.of_tag, "离线", "自动重连");

    lv_obj_t *ring = frame(scr, 91, 54, 58, 58);
    all_rule(ring, C_RED, 2, LV_RADIUS_CIRCLE);

    lv_obj_t *bar1 = panel(ring, 12, 25, 30, 4, C_RED, 2);
    lv_obj_set_style_transform_pivot_x(bar1, 15, 0);
    lv_obj_set_style_transform_pivot_y(bar1, 2, 0);
    lv_obj_set_style_transform_rotation(bar1, 450, 0);

    lv_obj_t *bar2 = panel(ring, 12, 25, 30, 4, C_RED, 2);
    lv_obj_set_style_transform_pivot_x(bar2, 15, 0);
    lv_obj_set_style_transform_pivot_y(bar2, 2, 0);
    lv_obj_set_style_transform_rotation(bar2, 3150, 0);

    s_ui.of_title = text(scr, &wb_font_28, C_RED, 0, 124, SCREEN_W, 34,
                         LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.of_title, "离线");

    s_ui.of_sub = text(scr, &buddy_font_16, C_INK, 0, 166, SCREEN_W, 20,
                       LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.of_sub, "未连接主机");

    s_ui.of_note = text(scr, &wb_font_12, C_DIM, SAFE_L, 192, CONTENT_W, 32,
                        LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_ui.of_note, LV_LABEL_LONG_WRAP);
    set_text(s_ui.of_note, "最后同步 --:-- · 缓存 0 条\n检查主机蓝牙与 daemon");

    build_footer(scr, SCR_OFFLINE);
}

void build_low_battery(void)
{
    lv_obj_t *scr = screen_new();
    s_ui.scr[SCR_LOW] = scr;
    build_header(scr, SCR_LOW, &s_ui.lb_idx, &s_ui.lb_tag, "电量", "0%");

    lv_obj_t *shell = panel(scr, 66, 58, 100, 44, C_BG, 6);
    all_rule(shell, C_RED, 3, 6);
    lv_obj_set_style_pad_all(shell, 4, 0);
    s_ui.lb_fill = panel(shell, 0, 0, 0, 30, C_RED, 2);

    panel(scr, 168, 73, 5, 14, C_RED, 2);

    s_ui.lb_pct = text(scr, &wb_font_28, C_RED, 0, 112, SCREEN_W, 34,
                       LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.lb_pct, "0%");

    s_ui.lb_title = text(scr, &buddy_font_16, C_INK, 0, 158, SCREEN_W, 20,
                         LV_TEXT_ALIGN_CENTER);
    set_text(s_ui.lb_title, "电量低 · 请尽快充电");

    s_ui.lb_note = text(scr, &wb_font_12, C_DIM, SAFE_L, 184, CONTENT_W, 32,
                        LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_ui.lb_note, LV_LABEL_LONG_WRAP);
    set_text(s_ui.lb_note, "长按 DOWN 关闭提醒\n连接保持，充电后自动恢复。");

    build_footer(scr, SCR_LOW);
    set_keys(SCR_LOW, "UP/DOWN", "屏蔽", "OK", "返回总览", "长按OK", "关机");
}

// ---------------- 数据 → 文本 ----------------

void format_hm(int64_t clock_s, char *out, size_t cap)
{
    if (clock_s <= 0) {
        snprintf(out, cap, "--:--");
        return;
    }
    time_t t = (time_t)clock_s;
    struct tm tmv;
    gmtime_r(&t, &tmv);   // t 已是主机本地墙钟，不能再套本地时区
    snprintf(out, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

const char *sess_name(const wb_view_t *v, int i)
{
    if (i < 0 || i >= v->n_sess) return "未命名会话";
    return v->sess[i].name[0] ? v->sess[i].name : "未命名会话";
}

void state_display(const char *token, const char **zh, uint32_t *color)
{
    struct Map {
        const char *token;
        const char *zh;
        uint32_t color;
    };
    static const Map k_map[] = {
        {"RUNNING", "运行中", C_MINT},
        {"IDLE", "空闲", C_DIM},
        {"COMPLETED", "已完成", C_GREEN},
        {"ERROR", "出错", C_RED},
        {"NEED_APPROVAL", "待审批", C_ORANGE},
        {"NEED_ANSWER", "待回答", C_ORANGE},
    };
    *zh = token && token[0] ? token : "空闲";
    *color = C_DIM;
    if (!token) return;
    for (const Map &m : k_map) {
        if (strcmp(token, m.token) == 0) {
            *zh = m.zh;
            *color = m.color;
            return;
        }
    }
    // 未知状态：直接显示主机给的 token（ASCII 在字体里），用薄荷色强调。
    *color = C_MINT;
}

// ---------------- 各屏刷新 ----------------

void update_overview(const wb_view_t *v)
{
    char buf[160];
    const int n = v->n_sess;
    int sel = v->sel;
    if (n > 0) {
        if (sel < 0) sel = 0;
        if (sel >= n) sel = n - 1;
    } else {
        sel = 0;
    }

    snprintf(buf, sizeof(buf), "%02d / %02d", n > 0 ? sel + 1 : 0, n);
    set_text(s_ui.ov_idx, buf);
    set_text(s_ui.ov_tag, "会话");

    const char *token = n > 0 ? v->sess[sel].state : "IDLE";
    const char *zh = nullptr;
    uint32_t color = C_MINT;
    state_display(token, &zh, &color);

    if (n > 0)
        snprintf(buf, sizeof(buf), "%s · 第 %d 条", token && token[0] ? token : "IDLE",
                 sel + 1);
    else
        snprintf(buf, sizeof(buf), "IDLE · 无会话");
    set_text(s_ui.ov_kick, buf);
    set_text(s_ui.ov_state, zh);
    ui_pixel_text_color(s_ui.ov_state, color);

    set_text(s_ui.ov_name, n > 0 ? sess_name(v, sel) : "暂无会话");

    if (n > 0) {
        const wb_sess_t *s = &v->sess[sel];
        // 协议已带每条会话的积分消耗，补在模型行尾（无数据时不占位）。
        char tail[32];
        if (s->credit > 0.0) {
            int tenths = (int)(s->credit * 10.0);
            snprintf(tail, sizeof(tail), " · %d.%d 分", tenths / 10, tenths % 10);
        } else {
            tail[0] = '\0';
        }
        if (s->model[0] && s->msg[0])
            snprintf(buf, sizeof(buf), "%s · %s%s", s->model, s->msg, tail);
        else if (s->model[0])
            snprintf(buf, sizeof(buf), "%s%s", s->model, tail);
        else if (s->msg[0])
            snprintf(buf, sizeof(buf), "%s%s", s->msg, tail);
        else
            snprintf(buf, sizeof(buf), "等待会话信息%s", tail);
    } else {
        snprintf(buf, sizeof(buf), "%s",
                 v->connected ? "等待主机下发状态" : "等待连接主机");
    }
    set_text(s_ui.ov_model, buf);

    for (int i = 0; i < WB_MAX_SESS; ++i) {
        uint32_t c = C_LINE;
        if (i < n) c = (i == sel) ? C_MINT : C_SEG_IDLE;
        set_color(s_ui.ov_seg[i], c);
    }

    if (n == 0)
        snprintf(buf, sizeof(buf), "等待主机下发会话");
    else if (sel + 1 < n)
        snprintf(buf, sizeof(buf), "下一条 · %s", sess_name(v, sel + 1));
    else
        snprintf(buf, sizeof(buf), "%s", sel == 0 ? "仅 1 条会话" : "已是最后一条");
    set_text(s_ui.ov_next, buf);

    set_keys(SCR_OVERVIEW, "UP", "上一条", "OK", (v->approval || v->ask) ? "打开待办" : "无待办", "DOWN", "下一条");

    if (sel != s_ui.last_sel || n != s_ui.last_n ||
        (token && (!s_ui.last_token_set || strcmp(token, s_ui.last_token) != 0))) {
        ESP_LOGI(TAG, "总览 %d/%d state=%s", n > 0 ? sel + 1 : 0, n,
                 token ? token : "-");
        s_ui.last_sel = sel;
        s_ui.last_n = n;
        snprintf(s_ui.last_token, sizeof(s_ui.last_token), "%s", token ? token : "");
        s_ui.last_token_set = token != nullptr;
    }
}

void update_approval(const wb_view_t *v)
{
    set_text(s_ui.ap_idx, "审批");
    set_text(s_ui.ap_tag, "待处理");
    set_text(s_ui.ap_tool, v->tool[0] ? v->tool : "WorkBuddy");
    set_text(s_ui.ap_meta, "等待确认");
    set_text(s_ui.ap_cmd, v->hint[0] ? v->hint : "主机请求执行操作");

    // 0=允许，1=拒绝。允许用实心薄荷，拒绝用红描边，避免两块实心抢注意力。
    const bool allow = v->sel == 0;
    lv_obj_t *a = s_ui.ap_act[0];
    lv_obj_set_style_bg_color(a, lv_color_hex(allow ? C_MINT : C_PANEL), 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
    all_rule(a, allow ? C_MINT : C_LINE, 2, 4);
    ui_pixel_text_color(s_ui.ap_act_txt[0], allow ? 0x000000 : C_DIM);

    lv_obj_t *d = s_ui.ap_act[1];
    lv_obj_set_style_bg_color(d, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    all_rule(d, allow ? C_LINE : C_RED, 2, 4);
    ui_pixel_text_color(s_ui.ap_act_txt[1], allow ? C_DIM : C_RED);

    set_keys(SCR_APPROVAL, "UP/DOWN", "选择", "OK", "确认", "长按DOWN", "拒绝");
}

void update_ask(const wb_view_t *v)
{
    const bool read_only = v->ask_ans == 0 || v->n_opts == 0;
    char buf[96];

    set_text(s_ui.aq_idx, "提问");
    set_text(s_ui.aq_tag, "1 / 1");

    snprintf(buf, sizeof(buf), "AskUserQuestion · %s",
             read_only ? "只读" : (v->ask_multi ? "多选" : "单选"));
    set_text(s_ui.aq_kick, buf);
    set_text(s_ui.aq_text, v->text[0] ? v->text : "主机需要你的回答");

    if (read_only) {
        set_text(s_ui.aq_ro, "请在电脑上回答");
        lv_obj_remove_flag(s_ui.aq_ro, LV_OBJ_FLAG_HIDDEN);
        set_text(s_ui.aq_hint, "");
        lv_obj_add_flag(s_ui.aq_hint, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < WB_OPT_MAX; ++i)
            lv_obj_add_flag(s_ui.aq_opt[i], LV_OBJ_FLAG_HIDDEN);
        set_keys(SCR_ASK, "UP/DOWN", "无操作", "OK", "无操作", "长按DOWN", "跳过");
        return;
    }

    lv_obj_add_flag(s_ui.aq_ro, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < WB_OPT_MAX; ++i) {
        if (i >= v->n_opts) {
            lv_obj_add_flag(s_ui.aq_opt[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ui.aq_opt[i], LV_OBJ_FLAG_HIDDEN);
        const bool selected = i == v->sel;
        left_rule(s_ui.aq_opt[i], selected ? C_MINT : C_LINE, 6);
        snprintf(buf, sizeof(buf), "%d", i + 1);
        set_text(s_ui.aq_num[i], buf);
        ui_pixel_text_color(s_ui.aq_num[i], selected ? C_MINT : C_DIM);
        set_text(s_ui.aq_opt_txt[i], v->opt[i]);
        ui_pixel_text_color(s_ui.aq_opt_txt[i], selected ? C_INK : C_DIM);
    }

    // 6 个选项时没有空间再放提示行，提示让位给键位栏。
    if (v->n_opts < WB_OPT_MAX) {
        int y = 112 + v->n_opts * 25 + 2;
        lv_obj_set_pos(s_ui.aq_hint, SAFE_L, y);
        lv_obj_remove_flag(s_ui.aq_hint, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "已选 %d / %d · OK 确认", v->sel + 1, v->n_opts);
        set_text(s_ui.aq_hint, buf);
    } else {
        lv_obj_add_flag(s_ui.aq_hint, LV_OBJ_FLAG_HIDDEN);
    }

    set_keys(SCR_ASK, "UP/DOWN", "移动焦点", "OK", "确认作答", "长按DOWN", "跳过");
}

void update_standby(const wb_view_t *v)
{
    char time_buf[16];
    char date_buf[40];

    format_hm(v->clock_s, time_buf, sizeof(time_buf));

    date_buf[0] = '\0';
    if (v->clock_s > 0) {
        time_t t = (time_t)v->clock_s;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        static const char *const k_week[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(date_buf, sizeof(date_buf), "%d月%02d日 周%s", tmv.tm_mon + 1,
                 tmv.tm_mday, k_week[tmv.tm_wday % 7]);
    } else {
        snprintf(date_buf, sizeof(date_buf), "--月--日");
    }

    set_text(s_ui.sb_top, v->connected ? "无人打扰 · 在线" : "等待主机连接");
    set_color(s_ui.sb_dot, v->connected ? C_MINT : C_DIM);
    set_text(s_ui.sb_clock, time_buf);
    set_text(s_ui.sb_date, date_buf);
}

void update_offline(const wb_view_t *v)
{
    char hm[16];
    char buf[80];
    format_hm(v->clock_s, hm, sizeof(hm));
    set_text(s_ui.of_idx, "离线");
    set_text(s_ui.of_tag, "自动重连");
    snprintf(buf, sizeof(buf), "最后同步 %s · 缓存 %d 条\n检查主机蓝牙与 daemon",
             hm, v->n_sess);
    set_text(s_ui.of_note, buf);
    set_keys(SCR_OFFLINE, "UP", "查看缓存", "OK", "回总览", "长按OK", "关机");
}

void update_low_battery(int soc)
{
    char buf[48];
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;

    snprintf(buf, sizeof(buf), "%d%%", soc);
    set_text(s_ui.lb_idx, "电量");
    set_text(s_ui.lb_tag, buf);
    set_text(s_ui.lb_pct, buf);

    int inner_w = 100 - 2 * 3 - 2 * 4;   // 86：外框 3px + padding 4px
    int fill_w = inner_w * soc / 100;
    if (fill_w < 3) fill_w = 3;
    lv_obj_set_width(s_ui.lb_fill, fill_w);
}

}  // namespace

// ---------------- 对外接口 ----------------

void wb_ui_init(void)
{
    if (s_ui.inited) return;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.screen = -1;
    s_ui.soc = -1;
    s_ui.last_sel = -1;
    s_ui.last_n = -1;
    s_ui.last_token_set = false;

    build_overview();
    build_approval();
    build_ask();
    build_standby();
    build_offline();
    build_low_battery();
    s_ui.inited = true;
}

void wb_ui_deinit(void)
{
    if (!s_ui.inited) return;
    for (int i = 0; i < SCR_COUNT; ++i) {
        if (s_ui.scr[i]) lv_obj_delete(s_ui.scr[i]);
    }
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.screen = -1;
    s_ui.soc = -1;
}

void wb_ui_set_battery(int soc)
{
    if (soc < 0) return;
    s_ui.soc = soc;
    if (soc > LOW_BATTERY_SOC) s_ui.low_dismissed = false;   // 充电恢复后，下次低电重新提醒
}

bool wb_ui_key(wb_ui_key_t key, bool long_press)
{
    if (!s_ui.inited) return false;
    (void)key;

    // 离线覆盖页：任意短按先回到缓存总览；长按 DOWN 等价“返回”。
    if (s_ui.screen == SCR_OFFLINE) {
        if (long_press) {
            if (key == WB_UI_KEY_OK) return false;   // 长按 OK 交给底座关机
            s_ui.offline_dismissed = true;
            return true;
        }
        if (key == WB_UI_KEY_OK) {
            s_ui.offline_dismissed = true;
            return true;
        }
        // UP/DOWN：按掉覆盖页，再把翻页交给业务层（缓存里继续翻）。
        s_ui.offline_dismissed = true;
        return false;
    }

    // 低电覆盖页：UP/DOWN 暂时屏蔽；OK / 长按 DOWN 返回总览。
    if (s_ui.screen == SCR_LOW) {
        if (long_press) {
            if (key == WB_UI_KEY_OK) return false;   // 长按 OK 交给底座关机
            if (key == WB_UI_KEY_DOWN) s_ui.low_dismissed = true;
            return true;
        }
        if (key == WB_UI_KEY_OK) {
            s_ui.low_dismissed = true;
            return true;
        }
        return true;
    }

    // 其余页面（含待机）由业务层处理按键。
    return false;
}

void wb_ui_sync(const wb_view_t *v, bool standby)
{
    if (!s_ui.inited || !v) return;

    if (v->connected) s_ui.offline_dismissed = false;

    const bool low = (s_ui.soc >= 0 && s_ui.soc <= LOW_BATTERY_SOC && !s_ui.low_dismissed);
    const bool offline = (!v->connected && !s_ui.offline_dismissed);

    int target = SCR_OVERVIEW;
    if (standby) {
        target = SCR_STANDBY;
    } else if (offline) {
        target = SCR_OFFLINE;
    } else if (low) {
        target = SCR_LOW;
    } else {
        switch (v->page) {
        case WB_PAGE_APPROVAL: target = SCR_APPROVAL; break;
        case WB_PAGE_ASK: target = SCR_ASK; break;
        default: target = SCR_OVERVIEW; break;
        }
    }

    if (target != s_ui.screen) {
        lv_screen_load(s_ui.scr[target]);
        ESP_LOGI(TAG, "screen -> %s", k_screen_name[target]);
        s_ui.screen = target;
        s_ui.last_sel = -1;
        s_ui.last_n = -1;
        s_ui.last_token_set = false;
    }

    // 顶栏连接点 + 电量在所有屏幕常驻，随每次快照刷新。
    for (int i = 0; i < SCR_COUNT; ++i)
        update_status_cluster(i, v->connected, s_ui.soc);

    switch (target) {
    case SCR_STANDBY: update_standby(v); break;
    case SCR_OFFLINE: update_offline(v); break;
    case SCR_LOW: update_low_battery(s_ui.soc); break;
    case SCR_APPROVAL: update_approval(v); break;
    case SCR_ASK: update_ask(v); break;
    default: update_overview(v); break;
    }
}
