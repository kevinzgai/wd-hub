// main/console/wb_view.c —— WD Hub 看板 7 页(240x320, LVGL 9)。
//
// 版式来源:xiozhi-workbuddy-board/wd-hub-design/WD-Hub-样式规范.md 第 6 节,
// 方向 A「仪表像素」。硬约束:
//   1) 页头 0..32 / 内容 38..288 / 页脚 4,294,232,22;卡片 x=4 w=232,间距 6,
//      圆角 4,1px 描边;3px 竖条是独立的 w=3 矩形,不是 border-left;
//   2) 每页只允许一个 montserrat_32 数字锚点,其余全部 buddy_font_16(行高 20);
//   3) 没有模糊/渐变/投影/位图/滚动容器/持续动画;翻页直接 lv_scr_load;
//   4) 颜色只从 wb_theme.h 的角色取,不在这里散落十六进制;
//   5) 状态不能只靠颜色 —— 每个状态点旁边都有文字。
//
// 不缓存 lv_screen_active();7 个 screen 由 console App 的 enter/exit 建删。
#include "wb_view.h"

#include <stdio.h>
#include <string.h>

#include "app_fonts.h"
#include "lvgl.h"
#include "ui_pixel.h"
#include "wd_core.h"
#include "wb_proto.h"
#include "wb_theme.h"

#define PAGE_N ((int)WB_VIEW_COUNT)

// 4px 点 + 5px 间距 + 16px 文字,不用胶囊底(规范 5.6)。
typedef struct {
    lv_obj_t *dot;
    lv_obj_t *label;
} wd_badge_t;

typedef struct {
    lv_obj_t *scr[PAGE_N];
    wd_badge_t hd[PAGE_N];        // 页头右侧链路状态(待机页没有)

    // 01 状态总览
    wd_badge_t st_level;
    lv_obj_t *st_remain;
    lv_obj_t *st_total;
    lv_obj_t *st_used;
    lv_obj_t *st_bar;
    lv_obj_t *st_sess_head;
    lv_obj_t *st_row_name[WB_MAX_SESS];
    wd_badge_t st_row_state[WB_MAX_SESS];
    lv_obj_t *st_row_credit[WB_MAX_SESS];
    lv_obj_t *st_clock;
    lv_obj_t *st_event;

    // 02 会话列表
    lv_obj_t *rk_head;
    lv_obj_t *rk_card[WB_MAX_SESS];
    lv_obj_t *rk_rail[WB_MAX_SESS];
    lv_obj_t *rk_name[WB_MAX_SESS];
    wd_badge_t rk_state[WB_MAX_SESS];
    lv_obj_t *rk_model[WB_MAX_SESS];
    lv_obj_t *rk_credit[WB_MAX_SESS];
    lv_obj_t *rk_msg[WB_MAX_SESS];

    // 03 今日消耗
    lv_obj_t *us_today;
    lv_obj_t *us_req;
    lv_obj_t *us_cache;
    lv_obj_t *us_quota;
    lv_obj_t *us_cache_bar;
    lv_obj_t *us_head;
    lv_obj_t *us_name[WB_USAGE_MODELS];
    lv_obj_t *us_bar[WB_USAGE_MODELS];
    lv_obj_t *us_val[WB_USAGE_MODELS];

    // 04 关于 / 链路
    wd_badge_t ab_badge;
    lv_obj_t *ab_state;
    lv_obj_t *ab_power;
    lv_obj_t *ab_frames;

    // 05 审批
    lv_obj_t *ap_tool;
    wd_badge_t ap_state;
    lv_obj_t *ap_cmd;
    lv_obj_t *ap_req;

    // 06 提问
    lv_obj_t *ak_tool;
    lv_obj_t *ak_text;
    lv_obj_t *ak_row[WB_OPT_MAX];
    lv_obj_t *ak_letter[WB_OPT_MAX];
    lv_obj_t *ak_opt[WB_OPT_MAX];
    lv_obj_t *ak_mark[WB_OPT_MAX];
    lv_obj_t *ak_rail[WB_OPT_MAX];   // 选中项左侧光标条（切换即移动）
    lv_obj_t *ak_fk;                 // 页脚左段（键名 / 状态，琥珀或绿）
    lv_obj_t *ak_fd;                 // 页脚右段（说明，灰）

    // 07 待机
    lv_obj_t *sb_clock;
    lv_obj_t *sb_date;
    lv_obj_t *sb_power;
    wd_badge_t sb_online;
} wb_view_t;

static wb_view_t s_ui;
static bool s_built;
static int s_loaded = -1;
static wb_view_page_t s_manual = WB_VIEW_STATUS;
static int s_ask_cursor;
static int s_last_ask_id = -1;

// 提问页的「已确认」与「撞到首/末项」两个软状态。只存 int / 时间戳，
// 不碰 LVGL，允许在按键上下文里直接写；渲染统一在 refresh_ask() 里做。
static int s_ask_commit_id;         // 已确认的题号，0 = 没有
static int s_ask_commit_idx = -1;   // 已确认的选项下标
static int s_ask_edge_dir;          // 0 无 / -1 撞首项 / +1 撞末项
static int64_t s_ask_edge_until_ms; // 边界提示的到期时刻

static int s_frames;
static char s_last_event[20] = "无";
static bool s_low_power;

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------
static void set_text(lv_obj_t *label, const char *txt)
{
    if (!label || !txt) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(label, txt);
}

// host 的动态文本可能自带 '\n';单行标签统一折成空格,避免 label 画出第二行压到下一块。
static void oneline(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        const char c = src[i];
        dst[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    dst[i] = '\0';
}

// 单行动态文本:内容不变时不重复 setText —— 否则 LVGL 每次都会把滚动位置复位。
// 只有文字实际宽度超过固定宽才开跑马灯,短文本保持 DOTS(不产生任何动画)。
static void set_roll(lv_obj_t *l, const char *txt)
{
    if (!l || !txt) return;
    char one[160];
    oneline(one, sizeof(one), txt);
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, one) == 0) return;
    lv_label_set_text(l, one);

    lv_point_t sz;
    lv_text_get_size(&sz, one, &buddy_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_label_set_long_mode(l, sz.x > lv_obj_get_width(l)
                                  ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                                  : LV_LABEL_LONG_MODE_DOTS);
}

// 实色矩形:0 圆角、无描边。3px 竖条 / 分隔线 / 状态点都走这里。
static lv_obj_t *mk_rect(lv_obj_t *parent, int x, int y, int w, int h,
                         uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

// 1px 描边面板:卡片(圆角 4)/ 页脚 / 摘要条 / 选项行(圆角 0)。
static lv_obj_t *mk_panel(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t bg, uint32_t border, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    return mk_panel(parent, x, y, w, h, WD_CARD, WD_BORDER, WD_RADIUS);
}

// 透明 flex 行:用于 4x4 点 + 文字的徽标、待机品牌位、页脚分段。
static lv_obj_t *mk_flex_row(lv_obj_t *parent, int x, int y, int w, int h,
                             int pad_col, bool align_end)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_column(o, pad_col, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, align_end ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return o;
}

static lv_obj_t *mk_text(lv_obj_t *parent, const char *text, uint32_t color,
                         int x, int y, int w)
{
    lv_obj_t *l = ui_pixel_label(parent, text, &buddy_font_16, color);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_obj_set_height(l, WD_LINE_H);   // 固定一行高,换行符不会再画出第二行
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);  // LVGL 9 的 LONG_DOT
    }
    return l;
}

static lv_obj_t *mk_text_at(lv_obj_t *parent, const char *text, uint32_t color,
                            int x, int y, int w, lv_text_align_t align)
{
    lv_obj_t *l = mk_text(parent, text, color, x, y, w);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

static lv_obj_t *mk_big(lv_obj_t *parent, const char *text, uint32_t color,
                        int x, int y, int w)
{
    lv_obj_t *l = ui_pixel_label(parent, text, &lv_font_montserrat_32, color);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    return l;
}

// 单行 + 跑马灯:固定宽高、左对齐,滚动参数只在这里设置一次。
static lv_obj_t *mk_roll_text(lv_obj_t *parent, const char *text, uint32_t color,
                              int x, int y, int w)
{
    lv_obj_t *l = mk_text(parent, text, color, x, y, w);
    lv_obj_set_style_anim_duration(l, 3000, 0);   // C3 上放慢,避免跑太快
    return l;
}

// 动态多行文本:用固定尺寸容器夹住高度(容器默认裁剪子对象),
// 避免 host 送来的长文本把第三行画到下一块内容上。
static lv_obj_t *mk_wrap_clip(lv_obj_t *parent, const char *text, uint32_t color,
                              int x, int y, int w, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_t *l = ui_pixel_label(box, text, &buddy_font_16, color);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *mk_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 1000);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, lv_color_hex(WD_MUTED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(WD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(WD_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 0, LV_PART_INDICATOR);
    return b;
}

static wd_badge_t mk_badge(lv_obj_t *parent, int x, int y, int w,
                           uint32_t color, bool align_end)
{
    wd_badge_t b;
    lv_obj_t *box = mk_flex_row(parent, x, y, w, WD_LINE_H, 5, align_end);
    b.dot = mk_rect(box, 0, 0, 4, 4, color);
    b.label = ui_pixel_label(box, "", &buddy_font_16, color);
    return b;
}

static void badge_set(wd_badge_t *b, const char *txt, uint32_t color)
{
    if (!b->label || !b->dot) return;
    set_text(b->label, txt);
    lv_obj_set_style_text_color(b->label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(b->dot, lv_color_hex(color), 0);
}

static void mk_header(int page, const char *title)
{
    lv_obj_t *scr = s_ui.scr[page];
    lv_obj_t *bar = mk_rect(scr, 0, 0, WD_SCR_W, WD_HEAD_H, WD_MUTED);
    mk_rect(scr, 0, WD_HEAD_H - 1, WD_SCR_W, 1, WD_BORDER);  // 页头底边 1px

    char code[16];
    snprintf(code, sizeof(code), "%d", page + 1);
    lv_obj_t *index_block = mk_rect(bar, 6, 7, 22, 18, WD_PRIMARY);
    lv_obj_t *num = ui_pixel_label(index_block, code, &buddy_font_16, WD_ON_PRIMARY);
    lv_obj_set_size(num, 22, 18);
    lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(num, LV_ALIGN_CENTER, 0, 0);

    mk_text(bar, title, WD_TEXT, 34, 6, 132);
    s_ui.hd[page] = mk_badge(bar, 170, 6, 64, WD_TEXT_DIM, true);
}

static void footer_seg(lv_obj_t *bar, const char *txt, uint32_t color)
{
    if (!txt || !txt[0]) return;
    ui_pixel_label(bar, txt, &buddy_font_16, color);
}

// 键名用琥珀、说明用灰;flex 居中,不写死像素宽度。
static void mk_footer(int page, const char *k1, const char *r1,
                      const char *k2, const char *r2)
{
    lv_obj_t *bar = mk_panel(s_ui.scr[page], WD_FOOT_X, WD_FOOT_Y, WD_FOOT_W,
                             WD_FOOT_H, WD_CARD, WD_BORDER, WD_RADIUS);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 0, 0);
    footer_seg(bar, k1, WD_PRIMARY);
    footer_seg(bar, r1, WD_TEXT_DIM);
    footer_seg(bar, k2, WD_PRIMARY);
    footer_seg(bar, r2, WD_TEXT_DIM);
}

// ---------------------------------------------------------------------------
// 状态映射
// ---------------------------------------------------------------------------
static const char *state_chip(const char *state)
{
    if (strcmp(state, "RUNNING") == 0) return "运行中";
    if (strcmp(state, "NEED_APPROVAL") == 0) return "待审批";
    if (strcmp(state, "NEED_ANSWER") == 0) return "待回答";
    if (strcmp(state, "COMPLETED") == 0) return "已完成";
    if (strcmp(state, "ERROR") == 0) return "出错";
    return "空闲";
}

// 规范 3.2:运行/完成=鼠尾草绿,等待/出错=珊瑚红,空闲=灰。颜色只做小面积。
static uint32_t state_color(const char *state)
{
    if (strcmp(state, "RUNNING") == 0) return WD_SUCCESS;
    if (strcmp(state, "NEED_APPROVAL") == 0 || strcmp(state, "NEED_ANSWER") == 0)
        return WD_DANGER;
    if (strcmp(state, "COMPLETED") == 0) return WD_SUCCESS;
    if (strcmp(state, "ERROR") == 0) return WD_DANGER;
    return WD_TEXT_DIM;
}

static uint32_t level_color(uint8_t level)
{
    if (level >= 2) return WD_DANGER;
    if (level == 1) return WD_PRIMARY;
    return WD_SUCCESS;
}

static void fmt_compact(char *buf, size_t n, double v)
{
    if (v >= 10000.0) snprintf(buf, n, "%.0fK", v / 1000.0);
    else if (v >= 1000.0) snprintf(buf, n, "%.1fK", v / 1000.0);
    else if (v >= 100.0) snprintf(buf, n, "%.0f", v);
    else snprintf(buf, n, "%.1f", v);
}

static void fmt_tokens(char *buf, size_t n, int64_t v)
{
    if (v < 0) snprintf(buf, n, "--");
    else if (v < 10000) snprintf(buf, n, "%lld", (long long)v);
    else if (v < 1000000) snprintf(buf, n, "%.0fK", (double)v / 1000.0);
    else if (v < 100000000) snprintf(buf, n, "%.1fM", (double)v / 1000000.0);
    else snprintf(buf, n, "%.0fM", (double)v / 1000000.0);
}

static void civil_date(int64_t days, int *pm, int *pd)
{
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp + (mp < 10 ? 3 : -9);
    *pm = (int)m;
    *pd = (int)d;
}

// ---------------------------------------------------------------------------
// 各页构建(坐标 = 规范第 6 节,单位 px,绝对坐标)
// ---------------------------------------------------------------------------
static void build_status(void)
{
    lv_obj_t *scr = s_ui.scr[WB_VIEW_STATUS];
    mk_header(WB_VIEW_STATUS, "状态总览");

    // 额度卡 4,38,232,92(3px 琥珀竖条)
    lv_obj_t *quota = mk_card(scr, 4, 38, 232, 92);
    mk_rect(quota, 0, 0, 3, 92, WD_PRIMARY);
    mk_text(quota, "剩余额度", WD_TEXT_DIM, 11, 6, 80);
    s_ui.st_level = mk_badge(quota, 162, 6, 62, WD_SUCCESS, true);
    s_ui.st_remain = mk_big(quota, "--", WD_PRIMARY, 11, 24, 130);
    s_ui.st_total = mk_text_at(quota, "--", WD_TEXT_DIM, 146, 26, 78, LV_TEXT_ALIGN_RIGHT);
    s_ui.st_used = mk_text_at(quota, "--", WD_TEXT_DIM, 146, 46, 78, LV_TEXT_ALIGN_RIGHT);
    s_ui.st_bar = mk_bar(quota, 11, 72, 214, 8);

    // 会话摘要卡 4,136,232,100
    lv_obj_t *sess = mk_card(scr, 4, 136, 232, 100);
    s_ui.st_sess_head = mk_text(sess, "0 项会话", WD_TEXT_DIM, 9, 8, 212);
    for (int i = 0; i < WB_MAX_SESS; i++) {
        const int y = 31 + i * 22;                       // 167 / 189 / 211
        s_ui.st_row_name[i] = mk_roll_text(sess, "", WD_TEXT, 9, y, 120);
        s_ui.st_row_state[i] = mk_badge(sess, 129, y, 58, WD_TEXT_DIM, true);
        s_ui.st_row_credit[i] =
            mk_text_at(sess, "", WD_PRIMARY, 187, y, 37, LV_TEXT_ALIGN_RIGHT);
    }

    // 主机 / 事件卡 4,242,232,44
    lv_obj_t *act = mk_card(scr, 4, 242, 232, 44);
    s_ui.st_clock = mk_text(act, "主机时间 未校时", WD_TEXT, 9, 4, 212);
    s_ui.st_event = mk_roll_text(act, "最近事件 无", WD_TEXT_DIM, 9, 24, 212);

    mk_footer(WB_VIEW_STATUS, "OK", " 总览 · ", "上下", " 翻页");
}

static void build_rack(void)
{
    lv_obj_t *scr = s_ui.scr[WB_VIEW_RACK];
    mk_header(WB_VIEW_RACK, "会话列表");

    // 摘要条 4,38,232,24
    lv_obj_t *bar = mk_panel(scr, 4, 38, 232, 24, WD_MUTED, WD_BORDER, WD_RADIUS);
    s_ui.rk_head = mk_text(bar, "0 项会话", WD_TEXT_DIM, 4, 2, 224);

    for (int i = 0; i < WB_MAX_SESS; i++) {
        const int y = 68 + i * 74;                       // 68 / 142 / 216
        lv_obj_t *card = mk_card(scr, 4, y, 232, 66);
        s_ui.rk_card[i] = card;
        s_ui.rk_rail[i] = mk_rect(card, 0, 0, 3, 66, WD_PRIMARY);
        lv_obj_add_flag(s_ui.rk_rail[i], LV_OBJ_FLAG_HIDDEN);
        s_ui.rk_name[i] = mk_roll_text(card, "", WD_TEXT, 11, 5, 130);
        s_ui.rk_state[i] = mk_badge(card, 146, 5, 78, WD_TEXT_DIM, true);
        s_ui.rk_model[i] = mk_roll_text(card, "", WD_TEXT_DIM, 11, 25, 140);
        s_ui.rk_credit[i] = mk_text_at(card, "", WD_PRIMARY, 152, 25, 72,
                                       LV_TEXT_ALIGN_RIGHT);
        s_ui.rk_msg[i] = mk_roll_text(card, "", WD_TEXT_DIM, 11, 45, 206);
    }

    mk_footer(WB_VIEW_RACK, "OK", " 总览 · ", "上下", " 翻页");
}

static void build_usage(void)
{
    lv_obj_t *scr = s_ui.scr[WB_VIEW_USAGE];
    mk_header(WB_VIEW_USAGE, "今日消耗");

    // KPI 卡 4,38,232,108(内容 x=15 → 3px 竖条)
    lv_obj_t *kpi = mk_card(scr, 4, 38, 232, 108);
    mk_rect(kpi, 0, 0, 3, 108, WD_PRIMARY);
    mk_text(kpi, "今日 tokens", WD_TEXT_DIM, 11, 6, 96);
    s_ui.us_today = mk_big(kpi, "--", WD_PRIMARY, 11, 24, 120);
    // 16px 中文下 "126 次请求" 约 72px，原 x=162 w=62 会截成 "次..."；
    // 左侧大数字占到 x=131，这里从 150 起给 80px，卡内右边界 230 < 232。
    s_ui.us_req = mk_text_at(kpi, "-- 次请求", WD_TEXT_DIM, 150, 6, 80,
                             LV_TEXT_ALIGN_RIGHT);
    s_ui.us_cache = mk_text_at(kpi, "缓存 --", WD_TEXT_DIM, 150, 26, 80,
                               LV_TEXT_ALIGN_RIGHT);
    s_ui.us_quota = mk_text_at(kpi, "额度 --", WD_TEXT_DIM, 150, 50, 80,
                               LV_TEXT_ALIGN_RIGHT);
    mk_text(kpi, "缓存命中", WD_TEXT_DIM, 11, 74, 64);
    s_ui.us_cache_bar = mk_bar(kpi, 76, 78, 146, 6);

    // 模型卡 4,152,232,134
    lv_obj_t *models = mk_card(scr, 4, 152, 232, 134);
    s_ui.us_head = mk_text(models, "模型用量", WD_TEXT_DIM, 9, 6, 212);
    for (int i = 0; i < WB_USAGE_MODELS; i++) {
        const int y = 28 + i * 20;                       // 180 / 200 / 220 / 240 / 260
        s_ui.us_name[i] = mk_roll_text(models, "", WD_TEXT, 9, y, 84);
        s_ui.us_bar[i] = mk_bar(models, 99, y + 1, 80, 6);
        s_ui.us_val[i] = mk_text_at(models, "", WD_PRIMARY, 185, y, 38,
                                    LV_TEXT_ALIGN_RIGHT);
    }

    mk_footer(WB_VIEW_USAGE, "OK", " 总览 · ", "上下", " 翻页");
}

static void build_about(void)
{
    lv_obj_t *scr = s_ui.scr[WB_VIEW_ABOUT];
    mk_header(WB_VIEW_ABOUT, "关于 / 链路");

    // 链路卡 4,38,232,132(正常=鼠尾草绿竖条;品牌唯一落点)
    lv_obj_t *link = mk_card(scr, 4, 38, 232, 132);
    mk_rect(link, 0, 0, 3, 132, WD_SUCCESS);
    mk_text(link, "WD HUB · 链路", WD_PRIMARY, 11, 4, 120);
    s_ui.ab_badge = mk_badge(link, 158, 4, 66, WD_SUCCESS, true);
    mk_rect(link, 9, 26, 216, 1, WD_BORDER);

    static const char *const k_link[5] = {"状态", "设备", "服务", "特征", "省电"};
    lv_obj_t *val[5];
    for (int i = 0; i < 5; i++) {
        const int y = 30 + i * 20;                       // 68 / 88 / 108 / 128 / 148
        mk_text(link, k_link[i], WD_TEXT_DIM, 9, y, 54);
        val[i] = mk_text(link, "", WD_TEXT, 63, y, 158);
    }
    lv_label_set_text(val[1], "Workbuddy BOX");          // BLE 广播名暂不随品牌改名
    lv_label_set_text(val[2], "4fafc201");
    lv_label_set_text(val[3], "beb5483e");
    s_ui.ab_state = val[0];
    s_ui.ab_power = val[4];

    // 固件卡 4,176,232,110
    lv_obj_t *fw = mk_card(scr, 4, 176, 232, 110);
    mk_text(fw, "固件", WD_TEXT, 9, 4, 100);
    // "1.3.0-passport" 约 112px，原 w=40 会截成 "1.3..."；左侧「固件」
    // 占到 x=109，从 110 起给 114px，右边界 224 < 卡宽 232。
    mk_text_at(fw, WB_FW_VERSION, WD_PRIMARY, 110, 4, 114, LV_TEXT_ALIGN_RIGHT);
    mk_rect(fw, 9, 26, 216, 1, WD_BORDER);

    static const char *const k_fw[3] = {"协议", "能力", "下行"};
    lv_obj_t *fv[3];
    for (int i = 0; i < 3; i++) {
        const int y = 30 + i * 24;                       // 206 / 230 / 254
        mk_text(fw, k_fw[i], WD_TEXT_DIM, 9, y, 54);
        fv[i] = mk_text(fw, "", WD_TEXT_DIM, 63, y, 158);
    }
    lv_label_set_text(fv[0], "2");
    lv_label_set_text(fv[1], "审批 / 提问");
    s_ui.ab_frames = fv[2];

    mk_footer(WB_VIEW_ABOUT, "OK", " 总览 · ", "上下", " 翻页");
}

static void build_approval(void)
{
    lv_obj_t *scr = s_ui.scr[WB_VIEW_APPROVAL];
    mk_header(WB_VIEW_APPROVAL, "命令审批");

    // 风险条 4,38,232,26
    lv_obj_t *risk = mk_rect(scr, 4, 38, 232, 26, WD_DANGER);
    mk_text_at(risk, "高危 · 需要确认", WD_ON_PRIMARY, 0, 3, 232, LV_TEXT_ALIGN_CENTER);

    // 命令卡 4,70,232,156
    lv_obj_t *card = mk_card(scr, 4, 70, 232, 156);
    s_ui.ap_tool = mk_roll_text(card, "-", WD_PRIMARY, 9, 6, 150);
    s_ui.ap_state = mk_badge(card, 158, 6, 66, WD_DANGER, true);
    mk_rect(card, 9, 28, 216, 1, WD_BORDER);
    s_ui.ap_cmd = mk_wrap_clip(card, "(没有命令原文)", WD_TEXT, 9, 36, 210, 60);
    s_ui.ap_req = mk_text(card, "请求编号 -", WD_TEXT_DIM, 9, 108, 214);
    mk_rect(card, 9, 132, 216, 1, WD_BORDER);
    mk_text(card, "不可撤销 · 请核对命令", WD_DANGER, 9, 135, 214);

    // 允许 4,232,232,26 / 拒绝 4,262,232,26(实底、等宽等高)
    lv_obj_t *allow = mk_rect(scr, 4, 232, 232, 26, WD_SUCCESS);
    mk_text_at(allow, "OK · 允许执行", WD_ON_PRIMARY, 0, 3, 232, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *deny = mk_rect(scr, 4, 262, 232, 26, WD_DANGER);
    mk_text_at(deny, "长按 DOWN · 拒绝", WD_ON_PRIMARY, 0, 3, 232, LV_TEXT_ALIGN_CENTER);

    mk_footer(WB_VIEW_APPROVAL, "OK", " 允许 · ", "长按DOWN", " 拒绝");
}

static void build_ask(void)
{
    lv_obj_t *scr = s_ui.scr[WB_VIEW_ASK];
    mk_header(WB_VIEW_ASK, "提问");

    // 问题卡 4,38,232,94
    lv_obj_t *card = mk_card(scr, 4, 38, 232, 94);
    s_ui.ak_tool = mk_roll_text(card, "-", WD_PRIMARY, 11, 6, 80);
    mk_text_at(card, "Q1", WD_TEXT_DIM, 186, 6, 38, LV_TEXT_ALIGN_RIGHT);
    s_ui.ak_text = mk_wrap_clip(card, "", WD_TEXT, 9, 28, 214, 40);
    mk_text(card, "长按 UP 取消作答", WD_TEXT_DIM, 9, 70, 214);

    // 选项 A-F:4,y,232,22,行距 2
    for (int i = 0; i < WB_OPT_MAX; i++) {
        const int y = 138 + i * 24;                      // 138 / 162 / 186 / 210 / 234 / 258
        lv_obj_t *row = mk_panel(scr, 4, y, 232, 22, WD_CARD, WD_BORDER, 0);
        s_ui.ak_row[i] = row;
        // 选中项左侧 2px 光标条：琥珀实心底上的一枚深色刻度，↑↓ 切换时随
        // 选中位置移动，给「当前项」一个除配色外的一级指示。未选中隐藏。
        s_ui.ak_rail[i] = mk_rect(row, 2, 2, 2, 18, WD_ON_PRIMARY);
        lv_obj_add_flag(s_ui.ak_rail[i], LV_OBJ_FLAG_HIDDEN);
        char key[2] = {(char)('A' + i), '\0'};
        s_ui.ak_letter[i] = mk_text(row, key, WD_TEXT_DIM, 6, 1, 14);
        s_ui.ak_opt[i] = mk_roll_text(row, "", WD_TEXT, 26, 1, 188);
        s_ui.ak_mark[i] = mk_rect(row, 222, 9, 4, 4, WD_TEXT_DIM);
    }

    // 页脚手建（不用 mk_footer）：文案要随「切换 / 撞到首末项 / 已提交 /
    // 暂无选项」四种状态变化，必须持有两个 label 的句柄。
    lv_obj_t *bar = mk_panel(scr, WD_FOOT_X, WD_FOOT_Y, WD_FOOT_W, WD_FOOT_H,
                             WD_CARD, WD_BORDER, WD_RADIUS);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 0, 0);
    s_ui.ak_fk = ui_pixel_label(bar, "OK 确认", &buddy_font_16, WD_PRIMARY);
    s_ui.ak_fd = ui_pixel_label(bar, " · ↑↓ 切换", &buddy_font_16, WD_TEXT_DIM);
}

static void build_standby(void)
{
    lv_obj_t *scr = s_ui.scr[WB_VIEW_STANDBY];
    ui_pixel_background(scr);
    // launcher 用的是 playful 主题，底色不是 travel 的 #181B1C，这里覆盖。
    lv_obj_set_style_bg_color(scr, lv_color_hex(WD_BG), 0);

    mk_rect(scr, 0, 0, WD_SCR_W, 2, WD_PRIMARY);

    // 品牌位:4x4 琥珀块 + WD HUB(字距 4),时钟上方,不抢时钟。
    // 容器用 20px 高(y=38..58,垂直中心仍是规范的 48),避免 16px 容器裁字。
    lv_obj_t *brand = mk_flex_row(scr, 0, 38, WD_SCR_W, WD_LINE_H, 4, false);
    mk_rect(brand, 0, 0, 4, 4, WD_PRIMARY);
    ui_pixel_label_ls(brand, "WD HUB", &buddy_font_16, WD_TEXT_DIM, 4);

    s_ui.sb_clock = mk_big(scr, "--:--", WD_PRIMARY, 0, 96, WD_SCR_W);
    lv_obj_set_style_text_align(s_ui.sb_clock, LV_TEXT_ALIGN_CENTER, 0);
    mk_rect(scr, 80, 136, 80, 1, WD_BORDER);
    s_ui.sb_date = mk_text_at(scr, "", WD_TEXT_DIM, 0, 148, WD_SCR_W,
                              LV_TEXT_ALIGN_CENTER);
    s_ui.sb_online = mk_badge(scr, 0, 186, WD_SCR_W, WD_SUCCESS, false);
    s_ui.sb_power = mk_text_at(scr, "", WD_TEXT_DIM, 0, 222, WD_SCR_W,
                               LV_TEXT_ALIGN_CENTER);
    mk_text_at(scr, "按任意键唤醒", WD_TEXT_DIM, 0, 274, WD_SCR_W,
               LV_TEXT_ALIGN_CENTER);
}

// ---------------------------------------------------------------------------
// 建 / 拆
// ---------------------------------------------------------------------------
// 本工程是 ESP32-C3 无 PSRAM：一次建齐 7 屏（约 490 个 LVGL 对象）会把
// BLE 初始化所需的堆吃光，实测 `E (BLE_INIT) Malloc failed` + 反复 panic。
// 按设计稿 §7.7 的生命周期改成「进屏才建、退屏即删」，任意时刻只驻留 1 屏。
// 上游 passport-workbuddy 工程内存宽裕，用的是一次建齐版本，这里不跟。
static void ensure_page(wb_view_page_t page)
{
    if (s_loaded == (int)page) return;

    // 先记住旧屏，建好并加载新屏之后再删，避免删掉正在显示的 screen。
    lv_obj_t *old = (s_loaded >= 0) ? s_ui.scr[s_loaded] : NULL;
    memset(&s_ui, 0, sizeof(s_ui));

    s_ui.scr[page] = lv_obj_create(NULL);
    lv_obj_remove_flag(s_ui.scr[page], LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_ui.scr[page]);
    lv_obj_set_style_bg_color(s_ui.scr[page], lv_color_hex(WD_BG), 0);

    switch (page) {
    case WB_VIEW_STATUS:   build_status();   break;
    case WB_VIEW_RACK:     build_rack();     break;
    case WB_VIEW_USAGE:    build_usage();    break;
    case WB_VIEW_ABOUT:    build_about();    break;
    case WB_VIEW_APPROVAL: build_approval(); break;
    case WB_VIEW_ASK:      build_ask();      break;
    case WB_VIEW_STANDBY:  build_standby();  break;
    default: break;
    }

    lv_scr_load(s_ui.scr[page]);
    s_loaded = (int)page;
    if (old) lv_obj_delete(old);
}

void wb_view_build(void)
{
    if (s_built) return;
    memset(&s_ui, 0, sizeof(s_ui));
    s_loaded = -1;
    s_ask_cursor = 0;
    // 屏是按需建/删的，重建后旧控件句柄全失效，交互态一并清零。
    s_ask_commit_id = 0;
    s_ask_commit_idx = -1;
    s_ask_edge_dir = 0;
    s_ask_edge_until_ms = 0;
    s_last_ask_id = -1;
    s_built = true;
    ensure_page(WB_VIEW_STATUS);
}

void wb_view_teardown(void)
{
    if (!s_built) return;
    if (s_loaded >= 0 && s_ui.scr[s_loaded]) {
        lv_obj_delete(s_ui.scr[s_loaded]);
    }
    memset(&s_ui, 0, sizeof(s_ui));
    s_built = false;
    s_loaded = -1;
}

void wb_view_set_manual_page(wb_view_page_t page)
{
    if (page <= WB_VIEW_ABOUT) s_manual = page;
}

wb_view_page_t wb_view_manual_page(void) { return s_manual; }

void wb_view_ask_move(int delta, int n_opts)
{
    if (n_opts <= 0) {
        s_ask_cursor = 0;
        return;
    }
    int c = s_ask_cursor + delta;
    while (c < 0) c += n_opts;
    while (c >= n_opts) c -= n_opts;
    s_ask_cursor = c;
}

int wb_view_ask_cursor(void) { return s_ask_cursor; }

// 适配层用：业务层的 sel（clamp 语义）才是提交时的选项下标，视觉游标必须
// 跟它对齐，不能让 view 内部的循环游标自己跑偏。
void wb_view_set_ask_cursor(int cursor)
{
    s_ask_cursor = cursor < 0 ? 0 : cursor;
}

void wb_view_set_ask_committed(int id, int idx)
{
    s_ask_commit_id = id > 0 ? id : 0;
    s_ask_commit_idx = s_ask_commit_id ? idx : -1;
}

void wb_view_set_ask_edge(int dir, int64_t until_ms)
{
    s_ask_edge_dir = (dir < 0) ? -1 : (dir > 0 ? 1 : 0);
    s_ask_edge_until_ms = s_ask_edge_dir ? until_ms : 0;
}

void wb_view_set_activity(int frames, const char *last_event, bool low_power)
{
    s_frames = frames;
    s_low_power = low_power;
    if (last_event) {
        snprintf(s_last_event, sizeof(s_last_event), "%s", last_event);
    }
}

wb_view_page_t wb_view_current_page(const wb_state_t *st)
{
    if (st->appr.active) return WB_VIEW_APPROVAL;
    if (st->ask.active) return WB_VIEW_ASK;
    if (st->standby_page) return WB_VIEW_STANDBY;
    return s_manual;
}

// ---------------------------------------------------------------------------
// 刷新
// ---------------------------------------------------------------------------
static void refresh_chrome(const wb_state_t *st)
{
    const bool linked = st->linked && !st->offline;
    const char *txt = linked ? "已连接" : (st->linked ? "离线" : "广播中");
    const uint32_t col = linked ? WD_SUCCESS : (st->linked ? WD_DANGER : WD_TEXT_DIM);

    for (int p = 0; p < (int)WB_VIEW_STANDBY; p++) {
        if (!s_ui.hd[p].label) continue;
        badge_set(&s_ui.hd[p], txt, col);
    }
}

static void refresh_status(const wb_state_t *st, int64_t now_ms)
{
    char buf[96];

    int pct = 0;
    if (st->credit.valid && st->credit.total > 0.0) {
        pct = (int)(st->credit.remain / st->credit.total * 100.0 + 0.5);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    lv_bar_set_value(s_ui.st_bar, pct * 10, LV_ANIM_OFF);

    if (st->credit.valid) {
        const uint32_t lc = level_color(st->credit.level);
        const char *lt = st->credit.level >= 2 ? "告急"
                          : st->credit.level == 1 ? "偏低" : "正常";
        badge_set(&s_ui.st_level, lt, lc);
        snprintf(buf, sizeof(buf), st->credit.remain >= 100.0 ? "%.0f" : "%.1f",
                 st->credit.remain);
    } else {
        badge_set(&s_ui.st_level, "未接入", WD_TEXT_DIM);
        snprintf(buf, sizeof(buf), "--");
    }
    set_text(s_ui.st_remain, buf);

    char t1[16] = "--", t2[16] = "--";
    if (st->credit.valid) {
        fmt_compact(t1, sizeof(t1), st->credit.total);
        fmt_compact(t2, sizeof(t2), st->credit.total - st->credit.remain);
    }
    snprintf(buf, sizeof(buf), "总额 %s", t1);
    set_text(s_ui.st_total, buf);
    snprintf(buf, sizeof(buf), "已用 %s", t2);
    set_text(s_ui.st_used, buf);

    const int running = wb_core_running_count(st);
    snprintf(buf, sizeof(buf), "%d 项会话 · %d 运行", st->n_sess, running);
    set_text(s_ui.st_sess_head, buf);

    for (int i = 0; i < WB_MAX_SESS; i++) {
        if (i >= st->n_sess) {
            lv_obj_add_flag(s_ui.st_row_name[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.st_row_state[i].dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.st_row_state[i].label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.st_row_credit[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ui.st_row_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.st_row_state[i].dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.st_row_state[i].label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.st_row_credit[i], LV_OBJ_FLAG_HIDDEN);

        const wb_sess_t *s = &st->sess[i];
        snprintf(buf, sizeof(buf), "%02d %s", i + 1, s->name[0] ? s->name : "(未命名)");
        set_roll(s_ui.st_row_name[i], buf);
        badge_set(&s_ui.st_row_state[i], state_chip(s->state), state_color(s->state));
        if (s->credit >= 0.0) snprintf(buf, sizeof(buf), "%.1f", s->credit);
        else snprintf(buf, sizeof(buf), "-");
        set_text(s_ui.st_row_credit[i], buf);
    }

    if (st->clock_local_s > 0) {
        const int64_t local_s = st->clock_local_s + (now_ms - st->clock_sync_ms) / 1000;
        const int sod = (int)(local_s % 86400);
        snprintf(buf, sizeof(buf), "主机时间 %02d:%02d:%02d", sod / 3600,
                 (sod % 3600) / 60, sod % 60);
    } else {
        snprintf(buf, sizeof(buf), "主机时间 未校时");
    }
    set_text(s_ui.st_clock, buf);

    snprintf(buf, sizeof(buf), "最近事件 %s", s_last_event);
    set_roll(s_ui.st_event, buf);
}

static void refresh_rack(const wb_state_t *st)
{
    char buf[96];
    int running = wb_core_running_count(st);
    double sum = 0.0;
    for (int i = 0; i < st->n_sess; i++) {
        if (st->sess[i].credit >= 0.0) sum += st->sess[i].credit;
    }
    snprintf(buf, sizeof(buf), "%d 项会话 · %d 运行 · %.1f 额度", st->n_sess, running,
             sum);
    set_text(s_ui.rk_head, buf);

    for (int i = 0; i < WB_MAX_SESS; i++) {
        lv_obj_t *card = s_ui.rk_card[i];
        if (i >= st->n_sess) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);

        const wb_sess_t *s = &st->sess[i];
        const bool run = strcmp(s->state, "RUNNING") == 0;
        // 选中卡:1px 琥珀描边 + 3px 琥珀竖条;未选中隐藏竖条(规范 5.4/5.6)
        lv_obj_set_style_border_color(card, lv_color_hex(run ? WD_PRIMARY : WD_BORDER), 0);
        if (run) lv_obj_remove_flag(s_ui.rk_rail[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.rk_rail[i], LV_OBJ_FLAG_HIDDEN);

        set_roll(s_ui.rk_name[i], s->name[0] ? s->name : "(未命名)");
        badge_set(&s_ui.rk_state[i], state_chip(s->state), state_color(s->state));
        set_roll(s_ui.rk_model[i], s->model[0] ? s->model : "模型 -");
        set_roll(s_ui.rk_msg[i], s->msg);
        if (s->credit >= 0.0) snprintf(buf, sizeof(buf), "%.1f 额度", s->credit);
        else snprintf(buf, sizeof(buf), "- 额度");
        set_text(s_ui.rk_credit[i], buf);
        lv_obj_set_style_text_color(s_ui.rk_credit[i],
                                    lv_color_hex(s->credit >= 0.0 ? WD_PRIMARY
                                                                  : WD_TEXT_DIM),
                                    0);
    }
}

static void refresh_usage(const wb_state_t *st)
{
    char buf[64];
    const wb_usage_t *u = &st->usage;

    fmt_tokens(buf, sizeof(buf), u->valid ? u->today : -1);
    set_text(s_ui.us_today, buf);
    if (u->valid) snprintf(buf, sizeof(buf), "%d 次请求", u->req);
    else snprintf(buf, sizeof(buf), "-- 次请求");
    set_text(s_ui.us_req, buf);

    if (u->valid && u->today > 0) {
        snprintf(buf, sizeof(buf), "缓存 %d%%", (int)((u->cached * 100) / u->today));
    } else {
        snprintf(buf, sizeof(buf), "缓存 --");
    }
    set_text(s_ui.us_cache, buf);

    if (st->credit.valid) snprintf(buf, sizeof(buf), "额度 %.1f", st->credit.remain);
    else snprintf(buf, sizeof(buf), "额度 --");
    set_text(s_ui.us_quota, buf);

    int cache_pct = 0;
    if (u->valid && u->today > 0) {
        cache_pct = (int)((u->cached * 100) / u->today);
        if (cache_pct > 100) cache_pct = 100;
        if (cache_pct < 0) cache_pct = 0;
    }
    lv_bar_set_value(s_ui.us_cache_bar, cache_pct * 10, LV_ANIM_OFF);

    int64_t max_tok = 0;
    for (int i = 0; i < u->n_model; i++) {
        if (u->model[i].tokens > max_tok) max_tok = u->model[i].tokens;
    }
    for (int i = 0; i < WB_USAGE_MODELS; i++) {
        if (u->valid && i < u->n_model && u->model[i].name[0]) {
            const wb_usage_model_t *m = &u->model[i];
            set_roll(s_ui.us_name[i], m->name);
            fmt_tokens(buf, sizeof(buf), m->tokens);
            set_text(s_ui.us_val[i], buf);
            int v = 0;
            if (max_tok > 0) v = (int)((m->tokens * 1000) / max_tok);
            if (v < 0) v = 0;
            if (v > 1000) v = 1000;
            lv_bar_set_value(s_ui.us_bar[i], v, LV_ANIM_OFF);
            // 前 2 名琥珀,其余灰(规范 5.5)
            lv_obj_set_style_bg_color(s_ui.us_bar[i],
                                      lv_color_hex(i < 2 ? WD_PRIMARY : WD_TEXT_DIM),
                                      LV_PART_INDICATOR);
        } else {
            set_roll(s_ui.us_name[i], "");
            set_text(s_ui.us_val[i], "");
            lv_bar_set_value(s_ui.us_bar[i], 0, LV_ANIM_OFF);
        }
    }
    set_text(s_ui.us_head, (u->valid && u->n_model > 0)
                               ? "模型用量" : (u->valid ? "今日暂无模型数据"
                                                        : "等待主机统计"));
}

static void refresh_about(const wb_state_t *st)
{
    char buf[64];
    const bool linked = st->linked && !st->offline;
    const char *txt = linked ? "已连接" : (st->linked ? "离线" : "广播中");
    const uint32_t col = linked ? WD_SUCCESS : (st->linked ? WD_DANGER : WD_TEXT_DIM);

    badge_set(&s_ui.ab_badge, txt, col);
    set_text(s_ui.ab_state, txt);
    lv_obj_set_style_text_color(s_ui.ab_state, lv_color_hex(col), 0);

    set_text(s_ui.ab_power, s_low_power ? "低功耗档" : "正常");
    lv_obj_set_style_text_color(s_ui.ab_power,
                                lv_color_hex(s_low_power ? WD_PRIMARY : WD_TEXT), 0);

    snprintf(buf, sizeof(buf), "%d 帧", s_frames);
    set_text(s_ui.ab_frames, buf);
}

static void refresh_approval(const wb_state_t *st)
{
    char buf[32];

    const wb_approval_t *a = &st->appr;
    set_roll(s_ui.ap_tool, a->tool[0] ? a->tool : "-");
    badge_set(&s_ui.ap_state, a->active ? "待审批" : "无",
              a->active ? WD_DANGER : WD_TEXT_DIM);
    set_text(s_ui.ap_cmd, a->hint[0] ? a->hint : "(没有命令原文)");

    if (a->id > 0) snprintf(buf, sizeof(buf), "请求编号 #%d", a->id);
    else snprintf(buf, sizeof(buf), "请求编号 -");
    set_text(s_ui.ap_req, buf);
}

static void refresh_ask(const wb_state_t *st, int64_t now_ms)
{
    const wb_ask_t *a = &st->ask;
    // 每道新题从第一项开始;同一题的重推(主机 30s 保活)不重置游标。
    if (a->id != s_last_ask_id) {
        s_last_ask_id = a->id;
        s_ask_cursor = 0;
    }
    set_roll(s_ui.ak_tool, a->tool[0] ? a->tool : "-");
    set_text(s_ui.ak_text, a->text[0] ? a->text : "(空提问)");

    if (s_ask_cursor >= a->n_opts) s_ask_cursor = a->n_opts > 0 ? a->n_opts - 1 : 0;

    // 已确认态：只认同一题；题号变了（新题 / 主机重推换了 id）自动失效。
    const bool committed = (s_ask_commit_id > 0 && a->id == s_ask_commit_id);
    const bool no_opt = (a->n_opts <= 0);
    const bool at_edge = (s_ask_edge_dir != 0 && now_ms < s_ask_edge_until_ms);

    for (int i = 0; i < WB_OPT_MAX; i++) {
        if (i >= a->n_opts) {
            lv_obj_add_flag(s_ui.ak_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ui.ak_row[i], LV_OBJ_FLAG_HIDDEN);

        bool active;
        uint32_t bg, border, fg;
        if (committed) {
            // 已提交：落选项转鼠尾草绿并保留反白字，其余选项压暗，
            // 明确「提交的是哪一项」，也防止再按 OK 造成二次上报。
            active = (i == s_ask_commit_idx);
            bg = active ? WD_SUCCESS : WD_CARD;
            border = active ? WD_SUCCESS : WD_BORDER;
            fg = active ? WD_ON_PRIMARY : WD_TEXT_DIM;
        } else {
            active = (i == s_ask_cursor);
            bg = active ? WD_PRIMARY : WD_CARD;
            border = active ? WD_PRIMARY : WD_BORDER;
            fg = active ? WD_ON_PRIMARY : WD_TEXT;
        }

        lv_obj_set_style_bg_color(s_ui.ak_row[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(s_ui.ak_row[i], lv_color_hex(border), 0);
        set_roll(s_ui.ak_opt[i], a->opt[i]);
        lv_obj_set_style_text_color(s_ui.ak_opt[i], lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(s_ui.ak_letter[i],
                                    lv_color_hex(active ? WD_ON_PRIMARY : WD_TEXT_DIM), 0);
        lv_obj_set_style_bg_color(s_ui.ak_mark[i],
                                  lv_color_hex(active ? WD_ON_PRIMARY : WD_TEXT_DIM), 0);
        if (active) lv_obj_remove_flag(s_ui.ak_rail[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.ak_rail[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 页脚四态：已提交 / 暂无选项 / 撞到首末项 / 正常切换提示。
    char buf[48];
    if (!s_ui.ak_fk || !s_ui.ak_fd) return;
    if (committed) {
        set_text(s_ui.ak_fk, "已提交");
        lv_obj_set_style_text_color(s_ui.ak_fk, lv_color_hex(WD_SUCCESS), 0);
        snprintf(buf, sizeof(buf), " · 选项 %d/%d", s_ask_commit_idx + 1, a->n_opts);
        set_text(s_ui.ak_fd, buf);
        lv_obj_set_style_text_color(s_ui.ak_fd, lv_color_hex(WD_TEXT_DIM), 0);
    } else if (no_opt) {
        set_text(s_ui.ak_fk, "暂无选项");
        lv_obj_set_style_text_color(s_ui.ak_fk, lv_color_hex(WD_DANGER), 0);
        set_text(s_ui.ak_fd, " · 等待主机下发");
        lv_obj_set_style_text_color(s_ui.ak_fd, lv_color_hex(WD_TEXT_DIM), 0);
    } else if (at_edge) {
        // 撞到首/末项：两段都用琥珀，和常态的「灰说明」拉开，一眼能认出
        // 「已经到顶了」而不是「按了没反应」。
        set_text(s_ui.ak_fk, "OK 确认");
        lv_obj_set_style_text_color(s_ui.ak_fk, lv_color_hex(WD_PRIMARY), 0);
        set_text(s_ui.ak_fd, s_ask_edge_dir < 0 ? " · 已到首项" : " · 已到末项");
        lv_obj_set_style_text_color(s_ui.ak_fd, lv_color_hex(WD_PRIMARY), 0);
    } else {
        set_text(s_ui.ak_fk, "OK 确认");
        lv_obj_set_style_text_color(s_ui.ak_fk, lv_color_hex(WD_PRIMARY), 0);
        snprintf(buf, sizeof(buf), " · ↑↓ 切换 %d/%d", s_ask_cursor + 1, a->n_opts);
        set_text(s_ui.ak_fd, buf);
        lv_obj_set_style_text_color(s_ui.ak_fd, lv_color_hex(WD_TEXT_DIM), 0);
    }
}

static void refresh_standby(const wb_state_t *st, int64_t now_ms)
{
    char buf[40];

    if (st->clock_local_s > 0) {
        const int64_t local_s = st->clock_local_s + (now_ms - st->clock_sync_ms) / 1000;
        const int sod = (int)(local_s % 86400);
        snprintf(buf, sizeof(buf), "%02d:%02d", sod / 3600, (sod % 3600) / 60);
        set_text(s_ui.sb_clock, buf);

        int mm, dd;
        civil_date(local_s / 86400, &mm, &dd);
        static const char *const k_week[] = {
            "周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        snprintf(buf, sizeof(buf), "%d月%02d日 %s", mm, dd,
                 k_week[(int)((local_s / 86400 + 4) % 7)]);
        set_text(s_ui.sb_date, buf);
    } else {
        set_text(s_ui.sb_clock, "--:--");
        set_text(s_ui.sb_date, "");
    }

    const bool linked = st->linked && !st->offline;
    badge_set(&s_ui.sb_online, linked ? "在线" : "离线",
              linked ? WD_SUCCESS : WD_DANGER);

    set_text(s_ui.sb_power, s_low_power ? "省电模式 · 亮度已降" : "低功耗待机");
}

void wb_view_refresh(const wb_state_t *st, int64_t now_ms)
{
    if (!s_built || !st) return;

    const wb_view_page_t page = wb_view_current_page(st);
    ensure_page(page);   // 按需建/拆屏，见 ensure_page() 的堆说明

    refresh_chrome(st);
    switch (page) {
    case WB_VIEW_STATUS:   refresh_status(st, now_ms); break;
    case WB_VIEW_RACK:     refresh_rack(st); break;
    case WB_VIEW_USAGE:    refresh_usage(st); break;
    case WB_VIEW_ABOUT:    refresh_about(st); break;
    case WB_VIEW_APPROVAL: refresh_approval(st); break;
    case WB_VIEW_ASK:      refresh_ask(st, now_ms); break;
    case WB_VIEW_STANDBY:  refresh_standby(st, now_ms); break;
    default: break;
    }
}
