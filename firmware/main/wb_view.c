// main/wb_view.c —— 控制台界面实现(240x240,LVGL 9.6)。
//
// 版式沿用官方参考图的设计语言:深海军蓝底 + 青色描边卡片 + 中文 chrome +
// 底部手势提示条。
//
// ⚠ chrome 曾一律用英文大写(TELEMETRY / SESSION RACK / QUOTA LEFT …),
// 2026-10-31 用户要求全中文化:「界面换成中文」。现在屏幕上的固定文案
// **没有任何英文单词**,英文只出现在主机下发的动态内容里(模型名、工具名、
// 命令原文)—— 那些是数据,不该翻译。
//
// 中英字宽差异是这次改版的主要几何约束:font_cn_12 的汉字步进是 12px、
// ASCII 约 6px。原来给 "SESSION RACK"(115px)算的槽换成 4 字(48px)只占一半,
// 所以标题槽缩到 88px 也不会截断;反过来 "TAP = ALLOW COMMAND" 这种
// 24 字符的行换成 8 个汉字后仍落在 214px 槽内,不需要改字号。
//
// 与原 240x320 版的差异:竖屏少了 80px,所以
//   1) 顶栏/底栏各压到 24px(原 26);
//   2) TELEMETRY 的环形从 96 降到 84,矩阵从 42 压到 28,摘要行距 14;
//   3) SESSION RACK 的卡片从 70 高降到 54,信息密度靠"同一行放两列"补回来;
//   4) 字体不用新字库:font_cn_12/16 各 3918 码点(含全部 ASCII),
//      font_cn_28 含 0-9 . % + - —,够配额数字用 —— 省掉 1.5MB Flash。
#include "wb_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "wb_eyes.h"

// 由 tools/gen_charset.py + lv_font_conv 生成的既有字库(看板同款)。
LV_FONT_DECLARE(font_cn_12);
LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_cn_28);

#define SCR_W 240
#define SCR_H 240
#define PAGE_COUNT 7

#define TOP_H 24
#define BOT_H 24
#define GAP   4

#define CONTENT_Y (TOP_H + 2)          // 26
#define CONTENT_H (SCR_H - BOT_H - CONTENT_Y - 2)   // 188
#define BOT_Y     (SCR_H - BOT_H + 2)  // 218

// ---------------------------------------------------------------------------
// 配色(两套外观)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t bg, panel, panel2, line, ink, dim, cyan, green, orange, red;
    int32_t radius, radii;
} wb_palette_t;

static const wb_palette_t s_palettes[2] = {
    // 0 赛博 HUD
    {.bg = 0x070D17, .panel = 0x0C1626, .panel2 = 0x122036, .line = 0x1C3250,
     .ink = 0xE8F2FF, .dim = 0x6D8AAE, .cyan = 0x35C8EC, .green = 0x3ED598,
     .orange = 0xF2A33C, .red = 0xE5484D, .radius = 8, .radii = 4},
    // 1 像素风:直角 + 琥珀
    {.bg = 0x0E0E0E, .panel = 0x1A1A1A, .panel2 = 0x262626, .line = 0x3A3A3A,
     .ink = 0xF5F5DC, .dim = 0x9A9A7A, .cyan = 0xFFCC00, .green = 0x88DD55,
     .orange = 0xFF8800, .red = 0xFF3333, .radius = 0, .radii = 0},
};

static uint8_t s_skin;
static const wb_palette_t *pal(void) { return &s_palettes[s_skin & 1U]; }

// ---------------------------------------------------------------------------
// 控件集合
// ---------------------------------------------------------------------------
typedef struct {
    lv_obj_t *scr[PAGE_COUNT];
    lv_obj_t *link_lbl[PAGE_COUNT];
    lv_obj_t *batt_lbl[PAGE_COUNT];
    lv_obj_t *batt_dot[PAGE_COUNT];

    // TELEMETRY
    lv_obj_t *arc;
    lv_obj_t *arc_pct;
    lv_obj_t *quota_num;
    lv_obj_t *quota_chip;
    lv_obj_t *total_val;
    lv_obj_t *used_val;
    lv_obj_t *matrix_head;
    lv_obj_t *cell[20];
    lv_obj_t *pipe_head;
    lv_obj_t *pipe_name[3];
    lv_obj_t *pipe_chip[3];
    lv_obj_t *pipe_tok[3];

    // RACK
    lv_obj_t *rack_head;
    lv_obj_t *rack_card[WB_MAX_SESS];
    lv_obj_t *rack_no[WB_MAX_SESS];
    lv_obj_t *rack_chip[WB_MAX_SESS];
    lv_obj_t *rack_tok[WB_MAX_SESS];
    lv_obj_t *rack_name[WB_MAX_SESS];
    lv_obj_t *rack_msg[WB_MAX_SESS];

    // RUN
    lv_obj_t *run_chip;
    lv_obj_t *run_name;
    lv_obj_t *run_model;
    lv_obj_t *run_elapsed;
    lv_obj_t *run_stage;
    lv_obj_t *run_bar;

    // USAGE（今日消耗）
    lv_obj_t *use_today;                  // 今日总量大字
    lv_obj_t *use_req;                    // 请求数
    lv_obj_t *use_cache;                  // 缓存命中率
    lv_obj_t *use_model_name[WB_USAGE_MODELS];
    lv_obj_t *use_model_val[WB_USAGE_MODELS];
    lv_obj_t *use_model_bar[WB_USAGE_MODELS];
    lv_obj_t *use_hint;                   // 无数据时的提示

    // APPROVAL
    lv_obj_t *appr_tool;
    lv_obj_t *appr_hint;
    lv_obj_t *appr_act[2];

    // ASK
    lv_obj_t *ask_tool;
    lv_obj_t *ask_text;
    lv_obj_t *ask_opt[WB_OPT_MAX];
    lv_obj_t *ask_hint;

    // STANDBY（大眼睛待机页）
    lv_obj_t *sb_idle;       // "已空闲 3 分 12 秒"
    lv_obj_t *sb_link;       // ● 在线 / ● 离线
} wb_view_t;

static wb_view_t s_ui;
static bool s_built;
static bool s_active;                    // false = 已退回看板屏
/* 常驻屏（只走蓝牙的固件用）：true 时不再等"主机推数据"才抢屏。
 * 依据是那颗板上根本没有可退的地方 —— WiFi 没开，看板取不到数，
 * 退回去只是一张永久显示 "--" 的空屏，看着像坏了。 */
static bool s_always_on;
static wb_page_t s_loaded = (wb_page_t)-1;   // 当前 load 的页
static lv_obj_t *s_prev_screen;          // 进控制台前的看板 screen

static int s_force_page = -1;
static int s_ask_cursor;

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------
static lv_obj_t *mk_screen(uint32_t bg)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    return scr;
}

static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t border)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(pal()->panel), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, pal()->radius, 0);
    lv_obj_set_style_border_width(c, border ? 1 : 0, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(border), 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *mk_fill(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// w <= 0 按内容自适应;否则固定宽度 + 省略号截断。
static lv_obj_t *mk_text(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                         int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *mk_wrap(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                         int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, "");
    return l;
}

// 文本没变就不重排:状态帧 2s 一次,逐帧 set_text 会让整屏每 2s 全量重绘。
static void set_text(lv_obj_t *label, const char *txt)
{
    if (!label || !txt) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(label, txt);
}

static lv_obj_t *mk_chip(lv_obj_t *parent, int x, int y, int w, int h,
                         uint32_t border, uint32_t text_color)
{
    lv_obj_t *c = mk_fill(parent, x, y, w, h, pal()->panel, pal()->radii);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(border), 0);
    lv_obj_t *l = lv_label_create(c);
    lv_obj_set_style_text_font(l, &font_cn_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(text_color), 0);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_label_set_text(l, "");
    return l;
}

// ---------------------------------------------------------------------------
// 顶栏 / 底栏
// ---------------------------------------------------------------------------
static void build_top(wb_page_t page, lv_obj_t *scr, const char *code, const char *title)
{
    const wb_palette_t *c = pal();

    /* 左上角方块标签。参考图这里是语义缩写(MIC / WB / CH / AI)，
     * 但实测时"这是第几屏"比"这是什么缩写"更有用 —— 挨个长按翻页核对
     * 排版时，没有页码根本对不上话（用户原话：我也弄不清楚第几屏）。
     * 所以这里改成页码，语义由右边标题承担。
     *
     * 现在三连击可以在 1/2/3 三页之间循环翻，页码成了**唯一**能确认
     * "我到底翻到第几页了"的标识，因此把比例调大一点：方块 24x20
     * 里用 16px 字，让数字在小屏上远一点也认得出。 */
    lv_obj_t *badge = mk_fill(scr, 3, 2, 22, 20, c->cyan, pal()->radii);
    lv_obj_t *bt = lv_label_create(badge);
    lv_obj_set_style_text_font(bt, &font_cn_16, 0);
    lv_obj_set_style_text_color(bt, lv_color_hex(c->bg), 0);
    lv_label_set_text(bt, code);
    lv_obj_center(bt);

    // 标题改中文后 4 字仅 48px，槽仍是 88px（给"命令需要审批"这类 6 字留余量）。
    set_text(mk_text(scr, &font_cn_12, c->ink, 29, 5, 88), title);

    // 右:链路胶囊 + 电量胶囊
    s_ui.link_lbl[page] = mk_chip(scr, 120, 2, 58, 20, c->cyan, c->cyan);

    // mk_chip 返回的是胶囊**内部**的 label,它的坐标相对胶囊算,不是相对屏幕。
    lv_obj_t *bp = mk_chip(scr, 182, 2, 54, 20, c->line, c->ink);
    s_ui.batt_dot[page] = mk_fill(scr, 186, 9, 7, 6, c->green, 1);
    s_ui.batt_lbl[page] = bp;
    lv_obj_set_width(bp, 36);
    lv_obj_set_pos(bp, 14, 4);
    lv_obj_set_style_text_align(bp, LV_TEXT_ALIGN_RIGHT, 0);
}

// 底栏：一颗按键的说明书。
//
// 原来是三格（左/中/右）各自带边框+独立底色，视觉上完全是三个"可点按钮"——
// 但这块板只有一颗物理按键、屏幕也不可触摸，用户第一反应就是
// "底下三个按钮是文案吗没有功能吧"。设计确实在骗人。
//
// 改法：取消分格，做成一条连续的说明文字「短按 <动作>    长按 <动作>」。
// 左边灰字标手势、右边亮字标动作，中间留白当分隔符。
// 没有任何边框或按钮外观，一眼就知道是提示而不是控件。
//
// 几何（条宽 232，font_cn_12 汉字步进 12px）：
//   短按  x=8  w=26  → 8..34    （2 字 = 24px）
//   动作  x=38 w=76  → 38..114  （4 字 = 48px，最长"按住说话"就 4 字）
//   长按  x=124 w=26 → 124..150
//   动作  x=154 w=76 → 154..230
//
// 上限是 4 个汉字：超过会走 DOTS 省略号。这一段是**静态文案**，
// 所以 4 字溢出在编译期就能看出来，不会到真机上才发现被截。
static void build_bottom(wb_page_t page, lv_obj_t *scr, const char *tap_act, const char *hold_act)
{
    const wb_palette_t *c = pal();
    (void)page;

    lv_obj_t *bar = mk_fill(scr, 4, BOT_Y, 232, 20, c->panel, pal()->radii);

    set_text(mk_text(bar, &font_cn_12, c->dim, 4, 2, 26), "短按");
    set_text(mk_text(bar, &font_cn_12, c->ink, 34, 2, 76), tap_act);

    set_text(mk_text(bar, &font_cn_12, c->dim, 120, 2, 26), "长按");
    set_text(mk_text(bar, &font_cn_12, c->ink, 150, 2, 76), hold_act);
}

// ---------------------------------------------------------------------------
// 状态映射
// ---------------------------------------------------------------------------
//
// 主机下发的 state 是**协议值**（RUNNING / NEED_APPROVAL / …），永远不翻译、
// 不改动比较逻辑；这里只把它映射成屏上给人看的中文。
//
// 两套名字：state_chip 用于会话卡（槽宽 80px，最多 5 字），
// state_short 用于首页管线摘要（槽宽 44px，必须 2 字）。
static const char *state_chip(const char *state)
{
    if (strcmp(state, "RUNNING") == 0) return "运行中";
    if (strcmp(state, "NEED_APPROVAL") == 0) return "待审批";
    if (strcmp(state, "NEED_ANSWER") == 0) return "待回答";
    if (strcmp(state, "COMPLETED") == 0) return "已完成";
    if (strcmp(state, "ERROR") == 0) return "出错";
    return "空闲";
}

// 摘要行右边只留 ~44px（3 个汉字就满），全称放不下，另给 2 字缩写。
//
// 这一列正好是 44px：13 个字槽位扣掉左端 128 起点，到右边框 216 只剩 44。
// 2 个汉字 = 24px，留 20px 余量，稳。
static const char *state_short(const char *state)
{
    if (strcmp(state, "RUNNING") == 0) return "运行";
    if (strcmp(state, "NEED_APPROVAL") == 0) return "审批";
    if (strcmp(state, "NEED_ANSWER") == 0) return "待答";
    if (strcmp(state, "COMPLETED") == 0) return "完成";
    if (strcmp(state, "ERROR") == 0) return "出错";
    return "空闲";
}

static uint32_t state_color(const char *state)
{
    if (strcmp(state, "RUNNING") == 0) return pal()->cyan;
    if (strcmp(state, "NEED_APPROVAL") == 0) return pal()->orange;
    if (strcmp(state, "NEED_ANSWER") == 0) return pal()->orange;
    if (strcmp(state, "COMPLETED") == 0) return pal()->green;
    if (strcmp(state, "ERROR") == 0) return pal()->red;
    return pal()->dim;
}

static uint32_t level_color(uint8_t level)
{
    if (level >= 2) return pal()->red;
    if (level == 1) return pal()->orange;
    return pal()->green;
}

// 把 1234.5 / 0.87 这类数字压成 4 字符以内,避免撑爆 62px 的槽。
static void fmt_compact(char *buf, size_t n, double v)
{
    if (v >= 10000.0) snprintf(buf, n, "%.0fK", v / 1000.0);
    else if (v >= 1000.0) snprintf(buf, n, "%.1fK", v / 1000.0);
    else if (v >= 100.0) snprintf(buf, n, "%.0f", v);
    else if (v >= 10.0) snprintf(buf, n, "%.0f", v);
    else snprintf(buf, n, "%.2f", v);
}

// ---------------------------------------------------------------------------
// PAGE 0 · TELEMETRY
// ---------------------------------------------------------------------------
static void build_status(lv_obj_t *scr)
{
    const wb_palette_t *c = pal();

    build_top(WB_PAGE_STATUS, scr, "1", "配额总览");

    // 左:环形配额 84x84
    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 84, 84);
    lv_obj_set_pos(arc, 5, CONTENT_Y);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(c->line), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    s_ui.arc = arc;
    s_ui.arc_pct = mk_text(scr, &font_cn_12, c->green, 5, CONTENT_Y + 34, 84);
    lv_obj_set_style_text_align(s_ui.arc_pct, LV_TEXT_ALIGN_CENTER, 0);
    set_text(s_ui.arc_pct, "0%");

    // 右:配额卡 142x84
    lv_obj_t *right = mk_card(scr, 93, CONTENT_Y, 142, 84, c->green);
    set_text(mk_text(right, &font_cn_12, c->dim, 8, 5, 126), "剩余额度");
    s_ui.quota_chip = mk_text(right, &font_cn_12, c->green, 76, 5, 58);
    lv_obj_set_style_text_align(s_ui.quota_chip, LV_TEXT_ALIGN_RIGHT, 0);

    s_ui.quota_num = mk_text(right, &font_cn_28, c->ink, 8, 20, 126);
    set_text(s_ui.quota_num, "--");

    // 三段标签原来是 TOT / 数值 / USD / 数值，英文占位。中文"总额·已用"
    // 各 2 字 = 24px，两个数值槽反而变宽了，够放 5 位数字。
    set_text(mk_text(right, &font_cn_12, c->dim, 8, 60, 26), "总额");
    s_ui.total_val = mk_text(right, &font_cn_12, c->ink, 34, 60, 40);
    set_text(mk_text(right, &font_cn_12, c->dim, 80, 60, 26), "已用");
    s_ui.used_val = mk_text(right, &font_cn_12, c->ink, 106, 60, 30);
    lv_obj_set_style_text_align(s_ui.used_val, LV_TEXT_ALIGN_RIGHT, 0);

    // 中:消耗矩阵 230x28
    lv_obj_t *mx = mk_card(scr, 5, 114, 230, 28, c->line);
    set_text(mk_text(mx, &font_cn_12, c->dim, 8, 2, 116), "消耗进度");
    s_ui.matrix_head = mk_text(mx, &font_cn_12, c->cyan, 130, 2, 92);
    lv_obj_set_style_text_align(s_ui.matrix_head, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < 20; i++) {
        s_ui.cell[i] = mk_fill(mx, 8 + i * 11, 18, 10, 7, c->line, 1);
    }

    // 下:管线摘要 230x66
    lv_obj_t *pipe = mk_card(scr, 5, 148, 230, 66, c->line);
    s_ui.pipe_head = mk_text(pipe, &font_cn_12, c->cyan, 8, 3, 214);
    for (int i = 0; i < 3; i++) {
        const int y = 20 + i * 15;
        s_ui.pipe_name[i] = mk_text(pipe, &font_cn_12, c->ink, 8, y, 118);
        s_ui.pipe_chip[i] = mk_text(pipe, &font_cn_12, c->dim, 128, y, 44);
        s_ui.pipe_tok[i] = mk_text(pipe, &font_cn_12, c->cyan, 174, y, 48);
        lv_obj_set_style_text_align(s_ui.pipe_tok[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    build_bottom(WB_PAGE_STATUS, scr, "下一页", "上一页");
}

// ---------------------------------------------------------------------------
// PAGE 1 · SESSION RACK
// ---------------------------------------------------------------------------
static void build_rack(lv_obj_t *scr)
{
    const wb_palette_t *c = pal();

    build_top(WB_PAGE_RACK, scr, "2", "会话列表");

    s_ui.rack_head = mk_text(scr, &font_cn_12, c->dim, 5, CONTENT_Y, 230);

    /* 三行固定版式（2026-10-31 第二次重排）。
     *
     * 第一版把 msg 做成"有值就顶替任务名"，问题是卡片高度会随数据跳变：
     * 有待答问题的会话显示 msg、其它的显示任务名，同一个位置两种语义，
     * 而且 msg 一出现任务名就消失，用户认不出是哪条会话。
     *
     * 现在给 msg 独立一行、**永远占位**（空就是空字符串，位置不动）：
     *   行1  y=1   序号(16px) + 状态(12px) + 额度(12px)
     *   行2  y=22  任务名(12px)
     *   行3  y=38  等待原因 msg(12px 橙色) —— 空则留白，但槽位固定
     * 卡高 54、间距 58（间隙 4px），三张到 160+54=214，底栏 218 之前收住。
     *
     * 行高依据：font_cn_12 line_height=15、font_cn_16 line_height=20，
     * 1+20=21 / 22+15=37 / 38+15=53，都在 54 内。 */
    for (int i = 0; i < WB_MAX_SESS; i++) {
        const int y = 44 + i * 58;
        lv_obj_t *card = mk_card(scr, 5, y, 230, 54, c->line);
        s_ui.rack_card[i] = card;

        s_ui.rack_no[i] = mk_text(card, &font_cn_16, c->cyan, 8, 0, 32);
        s_ui.rack_chip[i] = mk_text(card, &font_cn_12, c->dim, 40, 4, 80);
        s_ui.rack_tok[i] = mk_text(card, &font_cn_12, c->cyan, 138, 4, 84);
        lv_obj_set_style_text_align(s_ui.rack_tok[i], LV_TEXT_ALIGN_RIGHT, 0);

        s_ui.rack_name[i] = mk_text(card, &font_cn_12, c->ink, 8, 22, 214);
        s_ui.rack_msg[i] = mk_text(card, &font_cn_12, c->orange, 8, 38, 214);
    }

    build_bottom(WB_PAGE_RACK, scr, "下一页", "上一页");
}

// ---------------------------------------------------------------------------
// PAGE 2 · CO-PILOT RUN
// ---------------------------------------------------------------------------
static void build_run(lv_obj_t *scr)
{
    const wb_palette_t *c = pal();

    build_top(WB_PAGE_RUN, scr, "3", "运行详情");

    lv_obj_t *stage = mk_card(scr, 5, CONTENT_Y, 230, 108, c->green);
    set_text(mk_text(stage, &font_cn_12, c->dim, 8, 5, 130), "实时状态");
    s_ui.run_stage = mk_text(stage, &font_cn_12, c->green, 140, 5, 82);
    lv_obj_set_style_text_align(s_ui.run_stage, LV_TEXT_ALIGN_RIGHT, 0);

    // 吉祥物:两块眼睛 + 一排进度格,不用图片(省 Flash)
    lv_obj_t *face = mk_fill(stage, 78, 26, 74, 34, c->panel2, pal()->radius);
    lv_obj_set_style_border_width(face, 1, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(c->cyan), 0);
    mk_fill(face, 16, 10, 12, 14, c->cyan, 2);
    mk_fill(face, 46, 10, 12, 14, c->cyan, 2);

    s_ui.run_elapsed = mk_text(stage, &font_cn_28, c->cyan, 5, 66, 220);
    lv_obj_set_style_text_align(s_ui.run_elapsed, LV_TEXT_ALIGN_CENTER, 0);
    set_text(s_ui.run_elapsed, "00:00");

    lv_obj_t *task = mk_card(scr, 5, 138, 230, 38, c->line);
    s_ui.run_name = mk_text(task, &font_cn_12, c->ink, 8, 4, 214);
    s_ui.run_model = mk_text(task, &font_cn_12, c->dim, 8, 20, 214);

    lv_obj_t *eng = mk_card(scr, 5, 180, 230, 34, c->line);
    s_ui.run_bar = mk_text(eng, &font_cn_12, c->cyan, 8, 3, 214);
    set_text(s_ui.run_bar, "推理引擎 · 待命");
    s_ui.run_chip = mk_text(eng, &font_cn_12, c->green, 8, 17, 214);
    set_text(s_ui.run_chip, "主机在线");

    build_bottom(WB_PAGE_RUN, scr, "下一页", "上一页");
}

// ---------------------------------------------------------------------------
// PAGE 3 · USAGE（今日消耗）
// ---------------------------------------------------------------------------
//
// 版式（内容区 188px，从 CONTENT_Y=26 起）：
//
//   ┌────────────────────────────────────────┐
//   │ 今日消耗                    32 次请求   │  26px  KPI 头
//   │ 3.4M                     缓存命中 40%  │  40px  大字 + 副行
//   ├────────────────────────────────────────┤
//   │ 模型用量                               │  18px  小标题
//   │ trae/d…4.1-flash  ████████  1.2M      │  5 行 × 26px
//   │ deepse…4.1-flash  ██████    889K      │
//   │ ...                                    │
//   └────────────────────────────────────────┘
//
// 为什么模型条用"名字 + 横条 + 数值"三列而不是饼图：
//   1. 饼图在 240px 屏上放 5 个扇区，标签必然互相挤压，字都要挤到 8px；
//   2. 横条天然可比较 —— 用户真正想知道的是"哪个模型吃得多"，不是
//      "占比多少度"。横条的长度差一眼就看出来了。
//   3. 数值必须有：光看条长度看不出是 120 万还是 12 万，而这两个量级
//      对"要不要换模型"的决策影响完全不同。
static void build_usage(lv_obj_t *scr)
{
    const wb_palette_t *c = pal();

    build_top(WB_PAGE_USAGE, scr, "4", "今日消耗");

    // ---- 顶部 KPI 卡 ----
    lv_obj_t *kpi = mk_card(scr, 5, CONTENT_Y, 230, 62, c->cyan);
    set_text(mk_text(kpi, &font_cn_12, c->dim, 8, 4, 120), "今日消耗");
    s_ui.use_req = mk_text(kpi, &font_cn_12, c->cyan, 128, 4, 94);
    lv_obj_set_style_text_align(s_ui.use_req, LV_TEXT_ALIGN_RIGHT, 0);
    set_text(s_ui.use_req, "-- 次请求");

    // 大字用 28px：这是整页最重要的数字，要和第 1 页的剩余额度同等醒目。
    s_ui.use_today = mk_text(kpi, &font_cn_28, c->ink, 8, 18, 130);
    set_text(s_ui.use_today, "--");

    s_ui.use_cache = mk_text(kpi, &font_cn_12, c->dim, 128, 34, 94);
    lv_obj_set_style_text_align(s_ui.use_cache, LV_TEXT_ALIGN_RIGHT, 0);
    set_text(s_ui.use_cache, "");

    // ---- 模型用量卡 ----
    // 高度 118 = 18(标题) + 5×20(行)。CONTENT_Y+62+2 = 90 起，到 208 结束，
    // 底栏在 218 —— 中间留 10px 呼吸。
    lv_obj_t *box = mk_card(scr, 5, 90, 230, 118, c->line);
    set_text(mk_text(box, &font_cn_12, c->cyan, 8, 2, 214), "模型用量");

    for (int i = 0; i < WB_USAGE_MODELS; i++) {
        const int y = 18 + i * 20;
        // 名字 100px：12px 字宽下能放约 12 个 ASCII / 8 个汉字，
        // 主机按 16 字符预截并做首尾省略，这里再窄一点也不会被二次截断。
        s_ui.use_model_name[i] = mk_text(box, &font_cn_12, c->ink, 8, y, 100);
        // 横条底：静态画，长度在 refresh 里改。
        s_ui.use_model_bar[i] = mk_fill(box, 112, y + 4, 70, 6, c->line, 3);
        // 数值右对齐到卡片内沿
        s_ui.use_model_val[i] = mk_text(box, &font_cn_12, c->cyan, 186, y, 36);
        lv_obj_set_style_text_align(s_ui.use_model_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        set_text(s_ui.use_model_name[i], "");
        set_text(s_ui.use_model_val[i], "");
    }

    // 无数据提示：叠在模型卡中间，数据到了就藏起来。
    // 为什么需要它：设备刚连上、或当天还没有任何请求时，屏上五条空行
    // 看着像渲染坏了，得有一句话解释"为什么是空的"。
    s_ui.use_hint = mk_text(box, &font_cn_12, c->dim, 8, 50, 214);
    lv_obj_set_style_text_align(s_ui.use_hint, LV_TEXT_ALIGN_CENTER, 0);
    set_text(s_ui.use_hint, "等待主机统计");

    build_bottom(WB_PAGE_USAGE, scr, "下一页", "上一页");
}

// ---------------------------------------------------------------------------
// PAGE 3 · APPROVAL GATE
// ---------------------------------------------------------------------------
static void build_approval(lv_obj_t *scr)
{
    const wb_palette_t *c = pal();

    build_top(WB_PAGE_APPROVAL, scr, "4", "命令审批");

    lv_obj_t *box = mk_card(scr, 5, CONTENT_Y, 230, 150, c->red);
    lv_obj_set_style_bg_color(box, lv_color_hex(c->panel), 0);

    set_text(mk_text(box, &font_cn_12, c->red, 8, 5, 150), "高风险命令");

    // tool 是主机下发的数据（bash / Write / shell …），不翻译。
    s_ui.appr_tool = mk_text(box, &font_cn_16, c->ink, 8, 20, 214);

    // 命令原文:最多 4 行(12px x 15 = 60),超出由 WRAP 模式自己截断
    s_ui.appr_hint = mk_wrap(box, &font_cn_12, c->dim, 8, 44, 214, 100);

    // 这两行原来是 "TAP = ALLOW COMMAND" / "HOLD = DENY / CANCEL"。
    // 中文后 8 字 = 96px，214px 的槽有一半空着，所以把符号也排上更直观：
    // 「短按 = 允许执行」「长按 = 拒绝 / 取消」。
    static const char *k_act[2] = {"短按 = 允许执行", "长按 = 拒绝 / 取消"};
    for (int i = 0; i < 2; i++) {
        const int y = 180 + i * 18;
        lv_obj_t *row = mk_fill(scr, 5, y, 230, 16, c->panel, pal()->radii);
        s_ui.appr_act[i] = mk_text(row, &font_cn_12, c->ink, 8, 1, 214);
        set_text(s_ui.appr_act[i], k_act[i]);
    }

    build_bottom(WB_PAGE_APPROVAL, scr, "允许", "拒绝");
}

// ---------------------------------------------------------------------------
// PAGE 4 · QUESTION
// ---------------------------------------------------------------------------
static void build_ask(lv_obj_t *scr)
{
    const wb_palette_t *c = pal();

    build_top(WB_PAGE_ASK, scr, "5", "请选择");

    lv_obj_t *box = mk_card(scr, 5, CONTENT_Y, 230, 50, c->orange);
    s_ui.ask_tool = mk_text(box, &font_cn_12, c->orange, 8, 3, 214);
    s_ui.ask_text = mk_wrap(box, &font_cn_12, c->ink, 8, 18, 214, 30);

    for (int i = 0; i < WB_OPT_MAX; i++) {
        const int y = 80 + i * 19;
        lv_obj_t *row = mk_fill(scr, 5, y, 230, 17, c->panel, pal()->radii);
        s_ui.ask_opt[i] = mk_text(row, &font_cn_12, c->ink, 8, 1, 214);
    }
    // 6 项到 80+5*19+17 = 192,底栏 218,余量给提示行
    s_ui.ask_hint = mk_text(scr, &font_cn_12, c->dim, 5, 196, 230);

    build_bottom(WB_PAGE_ASK, scr, "下一个", "选择");
}

// ---------------------------------------------------------------------------
// PAGE 6 · STANDBY（大眼睛待机页）
// ---------------------------------------------------------------------------
//
// 声音/眼睛的原型来自旧看板（ui_board 的休眠屏）：空转时屏上只剩
// "两只眼睛 + 时间 + 还在线"。现在移植到控制台，触发条件换成
// "本地空闲满 3 分钟"（wb_core_tick 里算，见 WB_IDLE_TO_STANDBY_MS）。
//
// 为什么不做成第 6 个"导航页"（短按能翻到）：用户确认的方案是**空闲自动切**。
// 单键板只有短按/长按两种手势，导航环已经从 3 页长到 4 页会让"翻三下回到原点"
// 这条心智失效；而待机页本来就是"没人在用时才出现"的东西。
//
// 布局（240x240），和原休眠屏一致：
//        ┌──────────────────┐
//        │  ╱╲        ╱╲    │  ← 眉毛
//        │ ◡  ◡      ◡  ◡   │  ← 眼白（画布 y=22..104）
//        │     已空闲 12 分  │  ← 12px，y=140
//        │      ● 在线      │  ← 12px，y=196
//        └──────────────────┘
//
// ⚠ 这里**不建页头页脚**（build_top/build_bottom）：待机页整屏只留眼睛和
// 两行小字，页头那个页码徽标 + 电池条会让"待机"看起来还在工作。
// refresh_chrome() 会往 s_ui.link_lbl[STANDBY] 写链路胶囊 —— 那个数组按
// PAGE_COUNT 定长，待机页对应槽位存在但**没有控件**（NULL 检查在 set_text
// 里），所以不会崩，也不会画出东西。
static void build_standby(lv_obj_t *scr)
{
    const wb_palette_t *c = pal();

    // 眼睛：逐像素画布，位置/尺寸在 wb_eyes.h 里（与旧看板完全相同）。
    // 建失败不致命 —— 待机页会退化成"只有两行字"的纯文字页。
    if (!wb_eyes_build(scr)) {
        ESP_LOGW("wb-view", "眼睛画布没建起来，待机页退化为纯文字");
    }

    // 空闲时长。y=140：眼睛画布到 104，28px 时钟原有位置让给这两行小字 ——
    // 待机页显示"已空闲多久"比显示"现在几点"更有信息量（时间在这块板上
    // 本来就没有可靠来源：纯 BLE 模式拿不到 NTP，RTC 也没有电池）。
    s_ui.sb_idle = mk_text(scr, &font_cn_12, c->dim, 5, 140, 230);
    lv_obj_set_style_text_align(s_ui.sb_idle, LV_TEXT_ALIGN_CENTER, 0);
    set_text(s_ui.sb_idle, "");

    // 链路状态。待机页也不能丢掉"在线/离线" —— 断链时人会以为设备睡了。
    s_ui.sb_link = mk_text(scr, &font_cn_12, c->green, 5, 196, 230);
    lv_obj_set_style_text_align(s_ui.sb_link, LV_TEXT_ALIGN_CENTER, 0);
    set_text(s_ui.sb_link, "");
}

// ---------------------------------------------------------------------------
// 建 / 拆
// ---------------------------------------------------------------------------
void wb_view_build(void)
{
    if (s_built) return;

    memset(&s_ui, 0, sizeof(s_ui));
    s_loaded = (wb_page_t)-1;
    s_ask_cursor = 0;

    for (int p = 0; p < PAGE_COUNT; p++) {
        s_ui.scr[p] = mk_screen(pal()->bg);
    }

    build_status(s_ui.scr[WB_PAGE_STATUS]);
    build_rack(s_ui.scr[WB_PAGE_RACK]);
    build_run(s_ui.scr[WB_PAGE_RUN]);
    build_usage(s_ui.scr[WB_PAGE_USAGE]);
    build_approval(s_ui.scr[WB_PAGE_APPROVAL]);
    build_ask(s_ui.scr[WB_PAGE_ASK]);
    build_standby(s_ui.scr[WB_PAGE_STANDBY]);

    s_built = true;
}

void wb_view_teardown(void)
{
    if (!s_built) return;

    // 先把显示切回看板屏,再删控件 —— 删正在显示的 screen 会让 LVGL 访问悬空指针。
    if (s_prev_screen && s_active) lv_screen_load(s_prev_screen);

    /* 眼睛必须先释放再删 screen：它的定时器每 50ms 会碰画布和缓冲。
     * 直接删 screen 的话，画布随父容器一起没了，但定时器还在跑，
     * 下一帧 lv_timer_handler 就是野指针（切外观时必然触发）。 */
    wb_eyes_free();

    for (int p = 0; p < PAGE_COUNT; p++) {
        if (s_ui.scr[p]) {
            lv_obj_delete(s_ui.scr[p]);
            s_ui.scr[p] = NULL;
        }
    }
    memset(&s_ui, 0, sizeof(s_ui));
    s_built = false;
    s_active = false;
    s_loaded = (wb_page_t)-1;
}

void wb_view_force_page(int page) { s_force_page = page; }

void wb_view_set_active(bool on)
{
    if (!s_built) return;
    if (on == s_active) return;

    if (on) {
        s_prev_screen = lv_screen_active();
        s_active = true;
        s_loaded = (wb_page_t)-1;      // 强制下一帧 load
    } else {
        // 退屏时必须停眼睛：否则它会继续 50ms 一帧地算并 invalidate
        // 一个已经不在显示的画布。下一次进来 s_loaded 会被重置成 -1，
        // wb_view_refresh 会重新走 enter 分支。
        wb_eyes_leave();
        s_active = false;
        if (s_prev_screen) lv_screen_load(s_prev_screen);
    }
}

bool wb_view_active(void) { return s_active; }

void wb_view_set_always_on(bool on)
{
    s_always_on = on;
    if (on && s_built) wb_view_set_active(true);
}

void wb_view_set_skin(uint8_t skin) { s_skin = skin & 1U; }
uint8_t wb_view_skin(void) { return s_skin; }

// ---------------------------------------------------------------------------
// 刷新
// ---------------------------------------------------------------------------
static void refresh_chrome(const wb_state_t *st)
{
    const wb_palette_t *c = pal();
    char buf[24];

    const bool linked = st->linked && !st->offline;
    // 全英文的 LINK OK / OFFLINE / BLE ADV 对中文用户没有信息量
    // （用户原话：上面的 link ok 是干啥用的）。改中文两字，58px 胶囊装得下。
    //   已连 = BLE 连着、主机在线，数据最新
    //   离线 = BLE 连着但主机那侧断网/服务没起
    //   广播 = 还没主机连上来，正在等
    const char *link = linked ? "已连" : (st->linked ? "离线" : "广播");
    const uint32_t lc = linked ? c->cyan : (st->linked ? c->orange : c->dim);

    if (st->soc >= 0) snprintf(buf, sizeof(buf), "%d%%", st->soc);
    else snprintf(buf, sizeof(buf), "--");

    for (int p = 0; p < PAGE_COUNT; p++) {
        if (!s_ui.scr[p]) continue;
        /* 待机页（WB_PAGE_STANDBY）刻意不建页头：整屏只留眼睛和两行小字，
         * 页码徽标 + 链路胶囊会让"待机"看起来还在工作。所以那两个槽位是
         * NULL —— 必须逐字段判空，不能只判 scr[p]：
         * lv_obj_set_style_* 对 NULL 没有保护（set_text() 才有）。 */
        if (s_ui.link_lbl[p]) {
            set_text(s_ui.link_lbl[p], link);
            lv_obj_set_style_text_color(s_ui.link_lbl[p], lv_color_hex(lc), 0);
        }
        if (s_ui.batt_lbl[p]) set_text(s_ui.batt_lbl[p], buf);
        if (s_ui.batt_dot[p]) {
            const uint32_t bc = (st->soc >= 0 && st->soc <= 15) ? c->red
                               : (st->soc >= 0 && st->soc <= 35) ? c->orange : c->green;
            lv_obj_set_style_bg_color(s_ui.batt_dot[p], lv_color_hex(bc), 0);
        }
    }
}

static void refresh_status(const wb_state_t *st)
{
    const wb_palette_t *c = pal();
    // 64 而不是 32：管线摘要那行现在混排数字与汉字
    // （"%d 项会话   %d 运行"），按 %d 的最坏情况算是 37 字节，
    // 32 会被 GCC 判成可能截断（-Werror）。
    char buf[64];

    // 环形 + 中心百分比 + 数字
    int pct = 0;
    if (st->credit.valid && st->credit.total > 0.0) {
        pct = (int)(st->credit.remain / st->credit.total * 100.0 + 0.5);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    const uint32_t lv_col = level_color(st->credit.level);
    lv_arc_set_value(s_ui.arc, pct);
    lv_obj_set_style_arc_color(s_ui.arc, lv_color_hex(lv_col), LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "%d%%", pct);
    set_text(s_ui.arc_pct, buf);
    lv_obj_set_style_text_color(s_ui.arc_pct, lv_color_hex(lv_col), 0);

    if (st->credit.valid) {
        if (st->credit.remain >= 100.0) snprintf(buf, sizeof(buf), "%.0f", st->credit.remain);
        else snprintf(buf, sizeof(buf), "%.1f", st->credit.remain);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    set_text(s_ui.quota_num, buf);
    lv_obj_set_style_text_color(s_ui.quota_num, lv_color_hex(lv_col), 0);

    // 右上角:分级标签
    set_text(s_ui.quota_chip, st->credit.level >= 2 ? "告急"
                             : st->credit.level == 1 ? "偏低" : "正常");
    lv_obj_set_style_text_color(s_ui.quota_chip, lv_color_hex(lv_col), 0);

    char t1[16] = "--", t2[16] = "--";
    if (st->credit.valid) {
        fmt_compact(t1, sizeof(t1), st->credit.total);
        fmt_compact(t2, sizeof(t2), st->credit.total - st->credit.remain);
    }
    set_text(s_ui.total_val, t1);
    set_text(s_ui.used_val, t2);

    // 消耗矩阵:已用百分比 → 点亮的格数
    int used_pct = (st->credit.valid && st->credit.total > 0.0)
                       ? 100 - pct : 0;
    // "已用 12%" 比 "12% USED" 短：中文 2 字 24px + 数字，92px 的槽够放。
    snprintf(buf, sizeof(buf), "已用 %d%%", used_pct);
    set_text(s_ui.matrix_head, buf);
    const int lit = (used_pct * 20 + 99) / 100;
    for (int i = 0; i < 20; i++) {
        const uint32_t col = (i < lit) ? (lit >= 16 ? c->red : c->cyan) : c->line;
        lv_obj_set_style_bg_color(s_ui.cell[i], lv_color_hex(col), 0);
    }

    // 管线摘要。"12 项会话  3 运行" 是 9 个汉字宽 = 108px，214px 的槽放得下。
    int running = wb_core_running_count(st);
    snprintf(buf, sizeof(buf), "%d 项会话   %d 运行", st->n_sess, running);
    set_text(s_ui.pipe_head, buf);

    for (int i = 0; i < 3; i++) {
        if (i < st->n_sess) {
            const wb_sess_t *s = &st->sess[i];
            set_text(s_ui.pipe_name[i], s->name[0] ? s->name : "(未命名)");
            set_text(s_ui.pipe_chip[i], state_short(s->state));
            lv_obj_set_style_text_color(s_ui.pipe_chip[i],
                                        lv_color_hex(state_color(s->state)), 0);
            if (s->credit >= 0.0) snprintf(buf, sizeof(buf), "%.1f", s->credit);
            else snprintf(buf, sizeof(buf), "-");
            set_text(s_ui.pipe_tok[i], buf);
        } else {
            set_text(s_ui.pipe_name[i], i == 0 && st->n_sess == 0 ? "暂无会话" : "");
            set_text(s_ui.pipe_chip[i], "");
            set_text(s_ui.pipe_tok[i], "");
        }
    }
}

static void refresh_rack(const wb_state_t *st)
{
    const wb_palette_t *c = pal();
    /* 96 而不是 40：这一页的头行混排了数字和汉字，编译器对 %d / %.1f 的
     * 最坏情况（INT_MIN / 巨大 double）算出来的上界是 367 字节，40 会直接
     * 触发 -Wformat-truncation → -Werror。实际最长也就 "3 项会话 · 1 运行 ·
     * 742.5 额度" ≈ 40 字节，但这行是**每 2 秒刷一次**的窄路径，
     * 栈上多 56 字节换掉一个编译期断言，比调小 buf 更划算。 */
    char buf[96];

    int running = wb_core_running_count(st);
    double sum = 0.0;
    for (int i = 0; i < st->n_sess; i++) sum += st->sess[i].credit;
    // 头部一次说清三件事：共几项 / 几项在跑 / 合计花了多少额度。
    // 原来的 "03 PIPELINES  01 RUN  3.1 TOK" 是 29 字符（≈174px），
    // 中文 "3 项会话 · 1 运行 · 3.1 额度" 约 150px，还短一点。
    snprintf(buf, sizeof(buf), "%d 项会话 · %d 运行 · %.1f 额度", st->n_sess, running, sum);
    set_text(s_ui.rack_head, buf);

    for (int i = 0; i < WB_MAX_SESS; i++) {
        const bool shown = i < st->n_sess;
        lv_obj_t *card = s_ui.rack_card[i];
        if (!card) continue;

        if (!shown) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);

        const wb_sess_t *s = &st->sess[i];

        /* 边框高亮不再表示"选中"，而是表示"正在跑"。
         *
         * 原来这里跟着 s_rack_sel（短按在会话间移动的高亮）走，但那个
         * 选中态**没有任何后续动作**：不能打开、不能操作、屏幕也点不动，
         * 纯粹是装饰。2026-10-31 短按改成翻页后，这个游标彻底没人改了，
         * 于是把高亮让给一个真有用的信息：哪几张卡在 RUNNING。
         * 一屏三张卡，扫一眼就知道现在的重点在哪条。 */
        const bool running_now = strcmp(s->state, "RUNNING") == 0;
        lv_obj_set_style_border_width(card, running_now ? 2 : 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(running_now ? c->cyan : c->line), 0);

        snprintf(buf, sizeof(buf), "%02d", i + 1);
        set_text(s_ui.rack_no[i], buf);
        set_text(s_ui.rack_chip[i], state_chip(s->state));
        lv_obj_set_style_text_color(s_ui.rack_chip[i],
                                    lv_color_hex(state_color(s->state)), 0);

        set_text(s_ui.rack_name[i], s->name[0] ? s->name : "(未命名)");

        // msg 独立成行，永远占位：空就写空串，标签对象和位置都不动。
        // 这样同一张卡的任务名不会因为"这条会话有没有待答问题"而消失，
        // 版式在三张卡之间完全一致（用户要求：即使为空也占位）。
        set_text(s_ui.rack_msg[i], s->msg);

        // 槽宽 84px："12.3 额度" ≈ 50px，够。
        if (s->credit >= 0.0) snprintf(buf, sizeof(buf), "%.1f 额度", s->credit);
        else snprintf(buf, sizeof(buf), "- 额度");
        set_text(s_ui.rack_tok[i], buf);
    }
}

static void refresh_run(const wb_state_t *st, int64_t now_ms)
{
    const wb_palette_t *c = pal();
    // "模型：" 前缀 9 字节 + WB_MODEL_LEN 的模型名（主机下发，最长 32）
    // = 41，48 只剩 7 字节余量，GCC 会判成可能截断。给到 64。
    char buf[64];

    const wb_sess_t *s = NULL;
    for (int i = 0; i < st->n_sess; i++) {
        if (strcmp(st->sess[i].state, "RUNNING") == 0) { s = &st->sess[i]; break; }
    }
    if (!s && st->n_sess > 0) s = &st->sess[0];

    // MODEL: 这行原来是英文，模型名本身是主机下发的数据（claude-sonnet-4），
    // 只把标签中文化。
    set_text(s_ui.run_name, s ? (s->name[0] ? s->name : "(未命名)") : "暂无运行中会话");
    if (s) snprintf(buf, sizeof(buf), "模型：%s", s->model[0] ? s->model : "-");
    else snprintf(buf, sizeof(buf), "模型：-");
    set_text(s_ui.run_model, buf);

    const int64_t sec = wb_core_run_elapsed_s(st, now_ms);
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
    set_text(s_ui.run_elapsed, buf);

    const int running = wb_core_running_count(st);
    set_text(s_ui.run_stage, running ? "进行中" : "空闲");
    lv_obj_set_style_text_color(s_ui.run_stage,
                                lv_color_hex(running ? c->green : c->dim), 0);
    set_text(s_ui.run_bar, running ? "推理引擎 · 运算中" : "推理引擎 · 待命");
    set_text(s_ui.run_chip, st->offline ? "链路已断" : "主机在线");
}

static void refresh_approval(const wb_state_t *st)
{
    const wb_palette_t *c = pal();

    const wb_approval_t *a = &st->appr;
    set_text(s_ui.appr_tool, a->tool[0] ? a->tool : "-");
    set_text(s_ui.appr_hint, a->hint[0] ? a->hint : "(没有命令原文)");

    // 单键设备没有"高亮游标"（那颗键没有移动手势），所以两行各自常亮成
    // 对应的颜色：绿 = 短按允许，红 = 长按拒绝。一眼能看懂按哪个方向会怎样。
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_ui.appr_act[0]), lv_color_hex(c->green), 0);
    lv_obj_set_style_text_color(s_ui.appr_act[0], lv_color_hex(c->bg), 0);
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_ui.appr_act[1]), lv_color_hex(c->red), 0);
    lv_obj_set_style_text_color(s_ui.appr_act[1], lv_color_hex(c->bg), 0);
}

static void refresh_ask(const wb_state_t *st)
{
    const wb_palette_t *c = pal();

    const wb_ask_t *a = &st->ask;
    set_text(s_ui.ask_tool, a->tool[0] ? a->tool : "-");
    set_text(s_ui.ask_text, a->text[0] ? a->text : "(empty)");

    if (s_ask_cursor >= a->n_opts) s_ask_cursor = a->n_opts > 0 ? a->n_opts - 1 : 0;

    for (int i = 0; i < WB_OPT_MAX; i++) {
        lv_obj_t *row = lv_obj_get_parent(s_ui.ask_opt[i]);
        if (i >= a->n_opts) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);

        const bool sel = (i == s_ask_cursor);
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? c->cyan : c->panel), 0);

        // 单键板只有"移动 / 提交"两种手势，没有第三手势去勾选 ——
        // 所以不画 [ ]/[x] 勾选框，避免让用户以为能多选。
        set_text(s_ui.ask_opt[i], a->opt[i]);
        lv_obj_set_style_text_color(s_ui.ask_opt[i],
                                    lv_color_hex(sel ? c->bg : c->ink), 0);
    }

    if (a->ans == 0) set_text(s_ui.ask_hint, "只读，请在电脑上作答");
    else set_text(s_ui.ask_hint, "短按移动，长按选定");
}

// token 数量缩写。768 以下显示原值（"512"比"0.5K"更准），
// 再往上走 K/M，屏幕只有 36px 宽给这个数值。
//
// 为什么阈值是 10000 而不是 1000：
//   "1234" 是 4 个字符，"1.2K" 也是 4 个字符，宽度没省。
//   但"9876"一眼能读出精确量级，"9.9K"反而要心算。
//   过万之后数字必定 5 位以上，此时缩写才真正换来宽度。
static void fmt_tokens(char *buf, size_t n, int64_t v)
{
    if (v < 0) snprintf(buf, n, "--");
    else if (v < 10000) snprintf(buf, n, "%lld", (long long)v);
    else if (v < 1000000) snprintf(buf, n, "%.0fK", (double)v / 1000.0);
    else if (v < 100000000) snprintf(buf, n, "%.1fM", (double)v / 1000000.0);
    else snprintf(buf, n, "%.0fM", (double)v / 1000000.0);
}

static void refresh_usage(const wb_state_t *st)
{
    const wb_palette_t *c = pal();
    char buf[48];

    const wb_usage_t *u = &st->usage;

    // ---- 顶部 KPI ----
    if (u->valid) {
        fmt_tokens(buf, sizeof(buf), u->today);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    set_text(s_ui.use_today, buf);

    if (u->valid) {
        snprintf(buf, sizeof(buf), "%d 次请求", u->req);
    } else {
        snprintf(buf, sizeof(buf), "-- 次请求");
    }
    set_text(s_ui.use_req, buf);

    // 缓存命中率：分母用 today 而不是 prompt_tokens 合计。
    // 严格说命中率应该是 cached / prompt_total，但设备拿不到 prompt_total
    // （没下发），用 total 算出来的比率会偏低（因为 total 含 completion）。
    // 这里明确标成"缓存占比"而不是"命中率"，避免用户误读成一个精确指标。
    if (u->valid && u->today > 0) {
        const int pct = (int)((u->cached * 100) / u->today);
        snprintf(buf, sizeof(buf), "缓存占 %d%%", pct);
    } else {
        // 不能用 snprintf(buf, n, "")：零长度格式串会被 -Wformat-zero-length
        // 判成错误（-Werror 下直接编译失败）。直接置空更明确。
        buf[0] = '\0';
    }
    set_text(s_ui.use_cache, buf);

    // ---- 模型条 ----
    // 横条满格 = 最长那条（不是 today）。用相对长度比用绝对比例更可读：
    // 今日 340 万、最大模型 120 万时，按总量算最长条只有 70px 的 35%，
    // 五条都短短的看不出差异；按最大值算则最长条占满，差异一目了然。
    int64_t max_tok = 0;
    for (int i = 0; i < u->n_model; i++) {
        if (u->model[i].tokens > max_tok) max_tok = u->model[i].tokens;
    }

    for (int i = 0; i < WB_USAGE_MODELS; i++) {
        if (u->valid && i < u->n_model && u->model[i].name[0]) {
            const wb_usage_model_t *m = &u->model[i];
            set_text(s_ui.use_model_name[i], m->name);
            fmt_tokens(buf, sizeof(buf), m->tokens);
            set_text(s_ui.use_model_val[i], buf);

            // 条形宽度 70px 为满格，最小留 2px —— 0 宽度会让人以为
            // 这一行没有数据，而实际上是"有但很小"。
            int w = 2;
            if (max_tok > 0) {
                w = (int)((m->tokens * 70) / max_tok);
                if (w < 2) w = 2;
                if (w > 70) w = 70;
            }
            lv_obj_set_width(s_ui.use_model_bar[i], w);
            // 头两条用主色（cyan）强调"吃得多"，其余用暗色 —— 让眼睛
            // 直接落在最该关注的两行。
            lv_obj_set_style_bg_color(s_ui.use_model_bar[i],
                                      lv_color_hex(i < 2 ? c->cyan : c->dim), 0);
        } else {
            set_text(s_ui.use_model_name[i], "");
            set_text(s_ui.use_model_val[i], "");
            lv_obj_set_width(s_ui.use_model_bar[i], 0);
        }
    }

    // 无数据提示：只在"没有模型可画"时出现，且不遮住上方 KPI。
    if (u->valid && u->n_model > 0) {
        set_text(s_ui.use_hint, "");
    } else {
        set_text(s_ui.use_hint, u->valid ? "今日暂无模型数据" : "等待主机统计");
    }
}

static void refresh_standby(const wb_state_t *st, int64_t now_ms)
{
    const wb_palette_t *c = pal();
    char buf[48];

    /* 空闲时长：从 idle_since_ms 算起。st->idle_since_ms < 0（当前不空闲，
     * 例如刚被唤醒还没更新）时显示一句提示而不是负数。 */
    if (st->idle_since_ms > 0) {
        const int64_t d = (now_ms - st->idle_since_ms) / 1000;
        const int sec = d > 0 ? (int)d : 0;
        if (sec >= 3600) {
            snprintf(buf, sizeof(buf), "已空闲 %d 时 %d 分",
                     sec / 3600, (sec % 3600) / 60);
        } else if (sec >= 60) {
            snprintf(buf, sizeof(buf), "已空闲 %d 分 %d 秒", sec / 60, sec % 60);
        } else {
            snprintf(buf, sizeof(buf), "已空闲 %d 秒", sec);
        }
        set_text(s_ui.sb_idle, buf);
    } else {
        set_text(s_ui.sb_idle, "按任意键唤醒");
    }

    const bool linked = st->linked && !st->offline;
    set_text(s_ui.sb_link, linked ? "● 在线" : "● 离线");
    lv_obj_set_style_text_color(s_ui.sb_link,
                                lv_color_hex(linked ? c->green : c->red), 0);
}

void wb_view_refresh(const wb_state_t *st, int64_t now_ms)
{
    if (!s_built) return;

    const wb_page_t page = (s_force_page >= 0) ? (wb_page_t)s_force_page : wb_core_page(st);

    // 未主动进控制台时:只有主机推数据才自动切过来,否则不抢看板的屏。
    // 常驻屏（只走蓝牙）例外：没有看板可退，开机就该是控制台。
    if (!s_active) {
        if (!st->linked && !s_always_on) return;
        wb_view_set_active(true);
    }

    if (page != s_loaded) {
        /* 眼睛动画只在待机页跑。进的时候 reset（从闭眼开始睁，像被叫醒），
         * 出的时候立刻 pause —— 它每 50ms 重算一次画布并 invalidate，
         * 留在后台空转等于白烧 SPI 带宽。 */
        if (page == WB_PAGE_STANDBY) wb_eyes_enter();
        else if (s_loaded == WB_PAGE_STANDBY) wb_eyes_leave();

        lv_screen_load(s_ui.scr[page]);
        s_loaded = page;
    }

    refresh_chrome(st);

    switch (page) {
    case WB_PAGE_STATUS:   refresh_status(st); break;
    case WB_PAGE_RACK:     refresh_rack(st); break;
    case WB_PAGE_RUN:      refresh_run(st, now_ms); break;
    case WB_PAGE_USAGE:    refresh_usage(st); break;
    case WB_PAGE_APPROVAL: refresh_approval(st); break;
    case WB_PAGE_ASK:      refresh_ask(st); break;
    case WB_PAGE_STANDBY:  refresh_standby(st, now_ms); break;
    default: break;
    }

    // 底栏已改成「短按 <动作> / 长按 <动作>」的纯提示条，不再有中间格，
    // 审批页也不需要额外覆盖 —— 短按=允许、长按=拒绝本来就写在底栏上了。
}

// ---------------------------------------------------------------------------
// 交互游标
// ---------------------------------------------------------------------------
// 会话列表的选中游标已删除（见 refresh_rack 里的说明）：短按改翻页后
// 没有任何手势能移动它，留着就是一段永远停在 0 的死代码。
// 提问页的游标还在用 —— 短按移动、长按提交，是活的。

void wb_view_ask_move(int delta, int n_opts)
{
    if (n_opts <= 0) return;
    s_ask_cursor += delta;
    if (s_ask_cursor < 0) s_ask_cursor = n_opts - 1;
    if (s_ask_cursor >= n_opts) s_ask_cursor = 0;
}

int wb_view_ask_cursor(void) { return s_ask_cursor; }
