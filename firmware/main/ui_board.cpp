/* 看板 UI —— 实现（从 main.cpp 搬出，渲染逻辑与几何参数一字未改）
 *
 * 这里面的东西只跟"屏上画什么"有关：控件树、排版、颜色、判等缓存、
 * 切屏、休眠、浮层文案。取数在 data_fetch.cpp，硬件在 board_display.cpp。
 *
 * ⚠ 线程约定：所有碰 LVGL 的函数都必须在 LVGL 任务里调用。
 *   按键任务只调 ui_board_next_page / wake / brightness，那三个只写请求标志。
 */
#include "ui_board.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#include "lvgl.h"

#include "board_data.h"
#include "board_display.h"
#include "data_fetch.h"
#include "device_report.h"
#include "pet_view.h"
#include "text_util.h"
#include "battery.h"
#include "pet.h"
#include "power_save.h"
#include "wifi_mgr.h"

// 中文字体（由 tools/gen_charset.py + lv_font_conv 生成）
LV_FONT_DECLARE(font_cn_12);
LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_cn_28);
// 休眠屏的时钟从 72px 降到 28px（为了给眼睛让位），这个字号现在没有
// 使用点。声明留着是为了需要时一行就能切回去；实测链接器已把它整个
// 丢弃（nm build/wb-board.elf 查不到 font_cn_72），不占 Flash。
LV_FONT_DECLARE(font_cn_72);

static const char *TAG = "wb-board";

// ===========================================================================
// 配色
// ===========================================================================
//
// 深色底 + 高对比，小屏远看更清楚。
//
// 2026-10-26 整体提亮一轮。原因是 240x240 IPS 在强环境光下对比不足，
// 尤其 12px 的小字（会话副标题、页脚、日期、详情数值）远看糊成一片。
//
// 调整思路不是"把白色调更白" —— COL_TEXT 原来已是 16.35:1，足够。
// 真正的问题在辅助色 COL_DIM：它承载了大量 12px 文字，原值只有 5.18:1，
// 是整屏最低的一处，却用在最细的字上。所以：
//   1) COL_DIM 大幅提亮 #7D8590 -> #B4BEC9，对比度 5.18 -> 10.26
//   2) COL_TEXT 推到纯白，让主次层级进一步拉开
//   3) 彩色（蓝/绿/黄/红）各提一档，避免提亮灰阶后彩色显得发闷
//   4) 背景压深一点，底子更黑，所有前景色的实际观感对比都更强
//
// 数值为 WCAG 对比度（相对亮度比），括号内是调整前的值：
#define COL_BG      lv_color_hex(0x000000)   // 纯黑。原来 0x06080C 偏蓝灰，
                                             // 在全黑的 OLED 观感对比下会显出底色。
                                             // ⚠ 改这个必须同步重跑 tools/gen_logo.py，
                                             // 否则 logo 边缘会露出一圈 0x06080C 的方块。
#define COL_TEXT    lv_color_hex(0xFFFFFF)   // 20.0:1 (16.4)
#define COL_DIM     lv_color_hex(0xB4BEC9)   // 10.6:1 ( 5.2) <- 最关键的一处
#define COL_ACCENT  lv_color_hex(0x6FB6FF)   //  9.4:1 ( 7.7)
#define COL_OK      lv_color_hex(0x4FD463)   // 10.4:1 ( 7.6)
#define COL_WARN    lv_color_hex(0xE8B33A)   // 10.4:1 ( 7.7)
#define COL_ERR     lv_color_hex(0xFF6E66)   //  7.3:1 ( 5.8)

// ===========================================================================
// 几何常量
// ===========================================================================

// 图表屏几何。三个环横排：16 + 62×3 + 73×2 的间距算法实际是
// x = 16 / 89 / 162，最右到 224，左右各留 16 边距，视觉居中。
// 环心 y = 87 + 31 = 118，和设计稿一致。
#define CHART_ARC_SIZE   62
#define CHART_ARC_WIDTH   7
#define CHART_ARC_X0     16
#define CHART_ARC_STEP   73
#define CHART_ARC_Y      87
#define CHART_LABEL_Y   156     // 环下名称
#define CHART_FOOT_Y    185     // 底部进程/运行

// 环内百分比的居中方式：**不用 lv_obj_align_to**。
//
// 为什么不用（这是 LVGL 的语义陷阱，踩过一次）：
//   lv_obj_align_to 在 lv_obj_pos.c:521-522 是这么收尾的：
//       lv_obj_set_style_align(obj, LV_ALIGN_TOP_LEFT, 0);   // 对齐方式被抹掉
//       lv_obj_set_pos(obj, x, y);                           // 位置一次性写死
//   它用**调用那一刻** label 的宽度算一次位置，之后宽度变了也不会重算。
//   而标签刚 create 出来时宽度是 0（lv_label 的宽高默认 LV_SIZE_CONTENT，
//   lv_obj_constructor 里初始 coords 是 x2 = x1 - 1）。
//   → align_to 那一刻算出来的是 x = 环心 - 0/2 = 环心，
//     文字**左边缘正好顶在圆心**，整串字往右偏出去。
//   → 表现就是"文字起始点刚好在圆圈中间"，而且偏移量随位数变
//     （"19%" 偏 11px、"86%" 偏 15px、"100%" 偏 19px），看着忽左忽右。
//
// 改用显式宽高 + LV_TEXT_ALIGN_CENTER（LVGL 在框内自己居中），
// 位置用坐标算：文本框与环同宽同高同位置，框心即环心。
#define CHART_VALUE_BOX_W  CHART_ARC_SIZE

// 盒高取 20 让墨迹正好居中，垂直补偿量因此为 0：
//   font_cn_16 度量 line_height=20, base_line=4, 数字 box_h=12, ofs_y=0
//   墨迹顶 = (20-4) - 12 - 0 = 4，墨迹底(含) = 4+12-1 = 15 → 中心 9.5
//   盒高 20 的中心 = (20-1)/2 = 9.5  →  完全重合，不用补
// （早前按"墨迹底=16"算成偏下 0.5px，是闭区间少减了 1 写出界。）
#define CHART_VALUE_BOX_H  20

// 电量圆弧几何。
//
// 从 34px 缩到 24px：原来圆心里要放 "100%"（font_cn_12 的 ASCII 是 6px/字符，
// 三字符 18px，加上内径 26px 的下限才撑到 34）。现在**圆心里不放数字了**，
// 尺寸不再被文字撑住，纯按"一眼扫过去看得清弧长"来定。
//
// 24px + 弧宽 3px 的取舍：
//   内径 = 24 - 2*3 = 18px，中线圈半径 10.5px，周长 66px。
//   再小（20px）内径只剩 14px，圆弧看起来会像一段短横线而不是"环"；
//   24px 是还能看出"这是一个容量圆环"的下限，同时比原来小 30%，
//   页头右上角不再显眼 —— 电量是背景信息，不该和时钟抢注意力。
//
// y 方向：24px 在 0..36 的页头行里垂直居中 → y = (36-24)/2 = 6。
// 原来是 1（34px 几乎顶满），现在缩小后如果还是 y=1 会显得贴顶。
#define BAT_ARC_SIZE    24
#define BAT_ARC_WIDTH   3
#define BAT_ARC_X       (-7)            // 距右边 7px
#define BAT_ARC_Y       6
// 日期/状态右对齐的偏移 = 7(右边距) + 24(圆圈) + 5(间隔)
// 保持"不碰圆圈"的间隔感，间隔从 6 收到 5 是因为整体已经缩小了。
#define BAT_TEXT_RIGHT  (-36)

// 页头整行下移量。原来 logo 顶边贴 y=0，时钟（30px 行高）以 logo 中心对齐后
// 顶边落到 -3，被屏幕顶裁掉；下移 4px 后 logo 占 4..28、时钟 1..31，都完整可见。
#define HEADER_LOGO_Y   4

// 充电标记 "+" 的定位。
//
// 圆心的百分比数字删掉后，这个位置空了出来 —— 正好给充电标记用：
// 充电时圆心里一个绿色 "⚡"，不充电时什么都不显示。
// 比原来"圈外右上角挂个符号"更好读：圈外那个小符号在缩小到 24px 后
// 会飘得离环太远，看着像页面上多了一粒灰。
//
// 这里仍然不用 lv_obj_align_to（那是一次性计算，标签创建时宽度是 0，
// 会把文字左边缘顶在圆心上，详见图表常量处的说明）。
// 用"定尺寸文本框 + 文本居中 + 与圆圈同心"，字符变化不移位。
//
// 盒高取 15（= line_height），与圆圈垂直居中的补偿量：
//   (24 - 15) / 2 = 4（整数除），所以 dy = 4 → 盒占 10..24
//   墨迹中心约在 17.5，圆圈中心 18，差 0.5px，肉眼不可见
#define BAT_CHRG_BOX_H  15
#define BAT_CHRG_DY     ((BAT_ARC_SIZE - BAT_CHRG_BOX_H) / 2)

// 页脚右侧"★ 时间"。上一版为了给页脚里 20px 的宠物让位，这里改成过 -32；
// 宠物现已独立浮在右下角（不占页脚高度），让位不再需要，恢复 -8。
#define FOOTER_TEXT_RIGHT (-8)

// ===========================================================================
// LVGL 句柄
// ===========================================================================

static lv_obj_t *s_lbl_clock;
static lv_obj_t *s_lbl_date;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_stat[3];
static lv_obj_t *s_lbl_sess_title[MAX_SESSIONS];
static lv_obj_t *s_lbl_sess_meta[MAX_SESSIONS];
static lv_obj_t *s_dot[MAX_SESSIONS];        // 行首状态灯：等待操作时闪黄
static lv_obj_t *s_lbl_footer;
static lv_obj_t *s_lbl_sys;        // 页脚左侧：CPU / 内存 / 磁盘
static lv_obj_t *s_lbl_toast;      // 按键操作的临时提示
// 宠物（图 / 定时器 / 状态）归 pet_view.cpp，这里只调 pet_view_* 接口。

// ---- 电量圆圈（页头右上角）----
// 纯圆弧表示进度，**圆心不放百分比数字**。
// 数字让位给充电标记：充电时圆心显示 "+"，放电时圆心为空。
static lv_obj_t *s_arc_bat;        // 进度弧
static lv_obj_t *s_lbl_chrg;       // 圆心：充电时的 "⚡"，放电时为空

// ---- 多屏 ----
// 两组控件放在各自的容器里，切页只是切 HIDDEN，不销毁重建。
// 页头（logo/时钟/日期/状态）和页脚两组屏共用，所以留在 scr 上。
static lv_obj_t *s_pg_main;        // 第 0 屏：三格统计 + 会话列表
static lv_obj_t *s_pg_list;        // 第 1..N-1 屏：标题 + 6 行
static lv_obj_t *s_lbl_page;       // 详情屏标题
static lv_obj_t *s_lbl_row_t[MAX_ROWS];
static lv_obj_t *s_lbl_row_v[MAX_ROWS];

// 图表屏（系统资源）：三个环形仪表 + 名称 + 底部进程/运行
static lv_obj_t *s_pg_chart;
static lv_obj_t *s_lbl_chart_title;           // 页标题，和列表屏同位置
static lv_obj_t *s_arc_chart[MAX_CHARTS];
static lv_obj_t *s_lbl_chart_v[MAX_CHARTS];   // 环内百分比
static lv_obj_t *s_lbl_chart_k[MAX_CHARTS];   // 环下名称
static lv_obj_t *s_lbl_chart_l;               // 底部左：进程
static lv_obj_t *s_lbl_chart_r;               // 底部右：运行时长

// 哪几屏是图表屏。服务端只告诉固件"这一屏 kind=chart"，
// 切屏时还不知道类型 —— 首次切过去先按文本渲染，收到数据后记住，
// 以后切回来直接画图表，不会闪。开机第一次切系统屏能看到约一次
// HTTP 往返（<1s）的文本版，可以接受。
static bool s_page_is_chart[MAX_PAGES];

static volatile int s_page    = 0; // 当前屏号，按键切换
static volatile int s_page_req = -1; // 按键写入的切屏请求，由 LVGL 任务消费
// 屏数不在这里存了：它跟着服务端数据走，统一由 board_data_set_nscr() 管。
// 以前这里的 s_nscr 和 s_data.nscr 是两个副本，改一处忘一处就会"按出的屏数
// 和实际显示的屏数对不上"。取数任务句柄也归 data_fetch.cpp。

// ---- WiFi 状态浮层 ----
// 连接失败或配网中时盖住整屏。内容全部来自 wifi_mgr 的查询接口，
// 这边不做任何状态判断 —— 口径只有一处，改逻辑不用动 UI。
static lv_obj_t *s_pg_wifi;
static lv_obj_t *s_lbl_wifi_title;
static lv_obj_t *s_lbl_wifi_l1;
static lv_obj_t *s_lbl_wifi_l2;
static lv_obj_t *s_lbl_wifi_l3;

// ---- 服务地址浮层 ----
// WiFi 是通的、但连不上服务（多半是电脑换了 IP）时盖住整屏，
// 直接把配置页地址（设备自己的 IP）写在屏上：
//
//        服务连不上
//     192.168.0.89:27777     ← 现在用的地址
//       192.168.1.23         ← 配置页地址（绿）
//   用浏览器打开上面地址
//      进去改服务地址
//
// 和 WiFi 浮层分开建，是因为两者的触发条件互斥（一个要 WiFi 不通，
// 一个要 WiFi 通），合成一个状态机反而绕。
static lv_obj_t *s_pg_svr;
static lv_obj_t *s_lbl_svr_title;
static lv_obj_t *s_lbl_svr_l1;
static lv_obj_t *s_lbl_svr_l2;
static lv_obj_t *s_lbl_svr_tok;
static lv_obj_t *s_lbl_svr_l3;

// 标题只在内容变化时才 set_text —— 每次重设都会把跑马灯位置打回开头，
// 服务端 5 秒推一轮，不判等的话滚动会一直"闪回"。
static char     s_title_cache[MAX_SESSIONS][48];
static char     s_meta_cache[MAX_SESSIONS][64];
// 页脚右侧"多久之前"的显示缓存。ui_board_render 每帧都跑（约 50ms 一轮），
// 不判等就每帧 set_text 一次：每次都 malloc/free + 全量重绘，白烧 SPI。
static char     s_footer_cache[24];
// 芯片结温不在这里存：传感器句柄和最近读数都归 device_report.cpp，
// 渲染侧调 device_report_chip_temp() 读缓存（避免两个任务抢同一个句柄）。
static bool     s_waiting[MAX_SESSIONS];     // 当前哪些行在等待操作
// 实时状态：0空闲 / 1运行中（绿灯常亮）/ 2等你确认（黄灯闪）
// 为 0 时行首灯整颗隐藏，保持旧行行为。
static uint8_t  s_state[MAX_SESSIONS];
static bool     s_blink_on;                  // 黄灯亮/灭相位

// 详情屏的判等缓存（理由同上：每帧 set_text 会把 SPI 刷屏占满）
static char     s_row_t_cache[MAX_ROWS][TEXT_ROW];
static char     s_row_v_cache[MAX_ROWS][TEXT_ROWVAL];
static uint8_t  s_row_c_cache[MAX_ROWS];
static char     s_page_title_cache[32];

// ---- 按键状态（按键任务写，LVGL 任务读，避免跨任务碰 LVGL）----
// 升序排列：索引越大越亮。默认停在最亮那档。
static const uint8_t k_bright_steps[] = {40, 70, 100};
#define BRIGHT_STEP_NUM     (sizeof(k_bright_steps) / sizeof(k_bright_steps[0]))
#define BRIGHT_STEP_MAX     (BRIGHT_STEP_NUM - 1)
static volatile uint32_t s_bright_idx   = BRIGHT_STEP_MAX;  // 默认 100%
static volatile uint8_t  s_bright_apply = 0;   // 有新的亮度待应用
static volatile int16_t  s_toast_val    = -1;  // 待显示的亮度值，-1 = 不显示
static volatile uint32_t s_toast_until  = 0;   // 提示到期时刻（毫秒）
static volatile const char *s_last_btn  = "none";  // 最后按下的键，随轮询日志打出来

// ---- 空转休眠（Screen Off / 只显示时间）----
//
// 设计出发点：这块板子 7x24 挂在墙上，绝大部分时间是没活干的。
// 满屏的"0 工作 / 0 完成 / 无会话"配上 100% 背光，既费电又刺眼。
//
// 进入条件（三条同时成立）：
//   1. 连得上服务端（d->ok）—— 离线是故障，不该用休眠把它藏起来
//   2. 没有任务在跑（working == 0 && session_count == 0）
//   3. 人在第 0 屏（d->page == 0 && s_page == 0）
//
// 第 3 条不是可选的：/screen/<i> 只下发 rows，working/done/sessions 在
// parse_into 里 fallback 成 0。不加这个守卫，用户按键切到任何详情屏
// 都会被立刻判成"空闲"，画面直接跳回时钟屏 —— 按键等于失效。
// pet_view_update() 里有一模一样的守卫，原因相同。
//
// 退出：下一轮 /status 拿到 working>0 或有会话，自动恢复。
// 用户明确要"自动唤醒"，所以不绑按键 —— 按键在休眠时仍然可以切屏，
// 但切屏不会退出休眠（空闲状态下没有可看的内容）。
// volatile：LVGL 任务写、按键任务读。不加的话按键任务可能把值缓存在寄存器里，
// 永远看到"没睡"（或永远看到"睡着"），按键就时灵时不灵 —— 这种 bug 极难复现。
static volatile bool s_asleep = false;  // 当前是否处于省电模式
static lv_obj_t *s_pg_sleep;            // 休眠屏容器（全屏不透明）
static lv_obj_t *s_lbl_bigclock;        // 休眠屏：72px 大时钟
static lv_obj_t *s_lbl_sleep_dot;       // 休眠屏：在线状态点
// 休眠时背光降到这个值。30% 是"看得见但不刺眼"的下限 ——
// 再低（20%）在白天室内就快读不出时间了。
// 注意这不进 k_bright_steps：那是用户按键调节的档位，
// 休眠亮度是系统行为，不该出现在"☀ 40%"的提示里，也不该被按键改掉。
//
// ⚠ 2026-10-28 起背光由 power_save_set() 统一设置，这里只留注释说明
// 为什么是 30% —— 真正的值在 power_save.cpp 的 LIGHT_BRIGHT_PCT。
#define SLEEP_BRIGHT_PCT   30

// 空闲多久进**轻度**省电（背光 30% + 大时钟，但仍在 5 秒拉数据）。
//
// 原来是 60 秒，实测太急：服务端 working 计数带 30 分钟活跃窗口，
// 两个任务之间正常的思考间隙就会被判成空闲，屏幕反复睡/醒。
// 3 分钟能盖住绝大多数思考间隙，又不会让人走后白亮太久。
#define SLEEP_IDLE_MS      (3 * 60 * 1000)

// 空闲多久再降到**深度**省电（背光 5% + WiFi 最大省电 + 轮询 60 秒 + 2fps）。
//
// 10 分钟的判断依据：一个人离开工位去开会/吃饭，10 分钟是很自然的门槛。
// 这时候他确实不会回来看屏，深度省电的"响应变慢"代价可以接受；
// 而 3~10 分钟之间他随时可能回来瞄一眼，那时拉数据还得是 5 秒一轮。
//
// ⚠ 深睡**不是** deep sleep。设备一直在跑、WiFi 一直连着，
// 只是把"醒着的成本"压到最低。所以按 BOOT 1 秒内就能回到正常界面，
// 不用重连 WiFi、不用重拉数据 —— 这是相比真 deep sleep 的关键优势。
#define DEEP_IDLE_MS       (10 * 60 * 1000)

// 空闲计时起点。**不是**"醒着要保持到什么时候"，而是"从什么时候开始闲的"。
//
// 两种写法看起来差不多，但语义差很远：
//   保活制(deadline)  ：空闲 → 睡；按键 → 醒 + 定一个未来时间；到点又睡
//   计时制(since)     ：空闲开始计时；满 60 秒才睡；任何活动都清零重来
// 用户要的是后者（"没活动任务时候需要过一分钟才进省电模式"），
// 而且计时制天然满足"退出后再空闲满一分钟才进"，不用额外记状态。
static bool     s_idle_tracking = false;   // 是否正在计时
static uint32_t s_idle_since_ms = 0;       // 计时起点（毫秒）
// 按键任务写入的"请唤醒"请求，由 LVGL 任务消费（不能跨任务碰 LVGL）。
// 用独立标志而不是直接改 s_asleep：所有 UI 相关动作集中在 LVGL 任务里
// 落实更好排查，也避免两个任务同时写同一个状态。
static volatile bool s_wake_req = false;

// ===========================================================================
// 工具：建控件
// ===========================================================================

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          lv_color_t color, lv_align_t align,
                          lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, align, x, y);
    return l;
}

// 全屏透明容器：装一屏的控件，切页时整组 HIDDEN，比逐个控件改可见性可靠
static lv_obj_t *mk_page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 240, 240);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void mk_hline(lv_obj_t *parent, lv_coord_t y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, 224, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(line, COL_DIM, 0);
    // 底色压深后 OPA_30 的灰线几乎看不见了，提到 OPA_50 保持可见但不抢眼
    // （LVGL 只提供 10/20/30/40/50 这几档，没有更细的粒度）
    lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

// 黄灯相位翻转。只在 LVGL 任务里（lv_timer_handler）跑，和渲染同线程，
// 所以直接改控件样式不需要加锁。
static void blink_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_blink_on = !s_blink_on;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_dot[i]) continue;
        lv_obj_set_style_bg_opa(s_dot[i],
                                (s_waiting[i] && !s_blink_on) ? LV_OPA_20
                                                              : LV_OPA_COVER, 0);
    }
}

// ===========================================================================
// 建屏
// ===========================================================================

// ---- 服务地址浮层 ----
// 建在 WiFi 浮层之后 = 子对象顺序更靠上，两者同时可见时它盖住 WiFi 那层
// （理论上不会同时出现：一个要 WiFi 不通，一个要 WiFi 通）。
static void svr_ui_build(void)
{
    s_pg_svr = mk_page(lv_screen_active());
    lv_obj_set_style_bg_color(s_pg_svr, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_pg_svr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_pg_svr, LV_OBJ_FLAG_HIDDEN);

    s_lbl_svr_title = mk_label(s_pg_svr, &font_cn_28, COL_ERR,
                               LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(s_lbl_svr_title, "服务连不上");

    s_lbl_svr_l1 = mk_label(s_pg_svr, &font_cn_16, COL_TEXT,
                            LV_ALIGN_TOP_MID, 0, 100);
    lv_label_set_text(s_lbl_svr_l1, "");

    s_lbl_svr_l2 = mk_label(s_pg_svr, &font_cn_16, COL_OK,
                            LV_ALIGN_TOP_MID, 0, 132);
    lv_label_set_text(s_lbl_svr_l2, "");

    s_lbl_svr_l3 = mk_label(s_pg_svr, &font_cn_12, COL_DIM,
                            LV_ALIGN_TOP_MID, 0, 178);
    lv_label_set_text(s_lbl_svr_l3, "用浏览器打开上面地址\n进去改服务地址");
    lv_label_set_long_mode(s_lbl_svr_l3, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_svr_l3, 216);
    lv_obj_set_style_text_align(s_lbl_svr_l3, LV_TEXT_ALIGN_CENTER, 0);

    // 验证码。单独一个标签，因为它要**醒目**：用户得照着这 4 位数字
    // 填进网页表单，混在灰色说明文字里会被看漏。
    s_lbl_svr_tok = mk_label(s_pg_svr, &font_cn_16, COL_ACCENT,
                             LV_ALIGN_TOP_MID, 0, 206);
    lv_label_set_text(s_lbl_svr_tok, "");
}

// ===========================================================================
// 休眠屏的眼睛
// ===========================================================================
//
// 造型来自一张参考图，四个特征缺一不可：
//   1) 眼白不是椭圆，是横向椭圆被**一条斜睑线**从上方切掉之后的残月 ——
//      外角高、内角低、中段微微下凸。这条斜线是"眼睛"和"半个饼"的分界。
//   2) 瞳孔贴住上睑、被切掉上半，于是和纯黑背景连成一片。
//   3) 眼白下缘一条浅灰带，是"眼球厚度"，不是投影。
//   4) 瞳孔里一枚小白方块 —— 缺了它，瞳孔就只是个洞。
//   眉毛是独立一条暗色弧线，压在纯黑上只比背景亮一档，眉尾向外上挑。
//
// 为什么不用 lv_obj 拼：斜睑线是整个设计的核心，而圆角矩形只能做胶囊/圆角，
// 摆不出斜线（LVGL 9 也砍掉了对象旋转）。所以走 lv_canvas 逐像素画。
//
// 代价与对策：
//   画布 204x82 RGB565 = 33KB，落在 PSRAM；每帧只重算有内容的列。
//   **静止时不重算也不 invalidate** —— 待机屏绝大多数时间停在这个状态
//   （没眨眼、视线没漂、呼吸没跨过 1px），所以平均开销接近 0。
//   真正耗电的只有眨眼那 200ms 和视线漂移那 0.5s。

#define EYE_W          92      // 单眼宽（椭圆直径）
#define EYE_H          54      // 单眼高
#define EYE_R          20      // 瞳孔边长
#define EYE_HI          8      // 高光边长
#define EYE_CX_L       64      // 左眼中心 x
#define EYE_CX_R      176      // 右眼中心 x
#define EYE_CY         66      // 眼睛中心 y

// 上睑线两端相对椭圆上沿的下移量（全开时）
#define LID_OUT_OPEN    2      // 外角：几乎贴顶
#define LID_IN_OPEN    18      // 内角：压得低 → 斜线
#define LID_BULGE       1      // 中段下凸
// 闭合时相对椭圆下沿的上移量
#define LID_OUT_CLOSE   8
#define LID_IN_CLOSE    4

// 眉毛
#define BROW_THICK      3      // 线粗
#define BROW_GAP_OUT    4      // 外角处与睑线的间隙
#define BROW_GAP_IN    11      // 内角处间隙
#define BROW_LEVEL     58      // 灰度 0-255，压在纯黑上刚好能看出层次
#define BROW_TILT       5      // 眉尾上挑量

// 画布覆盖区域（设备坐标）。上下各留余量给呼吸浮动。
#define EYES_BX        18
#define EYES_BY        22
#define EYES_BW       204
#define EYES_BH        82

#define EYES_MS        50      // 20fps 节拍。眼睛不需要 25fps，省一点 SPI
#define EYE_CLOSE_MS   70
#define EYE_HOLD_MS    60
#define EYE_OPEN_MS    70
#define DOZE_AFTER_MS  (3 * 60 * 1000)   // 待机满 3 分钟开始犯困
#define EYE_PI         3.14159265f

static lv_obj_t   *s_cv_eyes   = NULL;
static lv_timer_t *s_eye_timer = NULL;
static uint16_t   *s_eye_buf   = NULL;

enum { EYES_IDLE, EYES_CLOSING, EYES_HOLD, EYES_OPENING };

static struct {
    float    open_pct, base_pct, phase_from;
    float    look_x, look_y, look_tx, look_ty;
    uint8_t  phase;
    uint32_t phase_ms, blink_in_ms, look_in_ms;
    uint32_t nod_in_ms, nod_left_ms, awake_ms, ms;
    bool     doze;
    // 上一帧真画出去的值，用来判断这一帧要不要重算
    float    last_open, last_lx, last_ly;
    int      last_breathe;
} s_eyes;

static inline float eye_clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float eye_min(float a, float b) { return a < b ? a : b; }
static inline float eye_max(float a, float b) { return a > b ? a : b; }
static inline float eye_lerp(float a, float b, float p) { return a + (b - a) * p; }

// 灰度 → RGB565（r/g/b 同值）。LVGL 9 的 RGB565 缓冲是 native 小端 uint16，
// 显示层如果是 SWAPPED 格式，由 LVGL 绘制时自己换 —— 这里不用管。
static inline uint16_t eye_grey(int v)
{
    uint16_t g = (uint16_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    return (uint16_t)(((g & 0xF8) << 8) | ((g & 0xFC) << 3) | (g >> 3));
}

static inline void eye_put(int x, int y, int v)
{
    if (x < 0 || x >= EYES_BW || y < 0 || y >= EYES_BH) return;
    s_eye_buf[y * EYES_BW + x] = eye_grey(v);
}

// 像素 [y, y+1) 与区间 [a,b] 的重叠比例，就是抗锯齿用的覆盖率。
// 逐像素画之所以还能不锯齿，全靠这个 —— 边界那一行按覆盖比例混色。
static inline float eye_cover(int y, float a, float b)
{
    float lo = eye_max((float)y, a);
    float hi = eye_min((float)y + 1.0f, b);
    return hi > lo ? hi - lo : 0.0f;
}

// ---- 眉毛 ----
// 基准取"全开"时的睑线，不跟眨眼上下动。真实眨眼时眉毛几乎不动，
// 若让它跟着睑线压，整张脸会像在抽搐。
static void eye_draw_brow(int cx, int cy, bool mirror)
{
    const float rw  = EYE_W * 0.5f;
    const float top = (float)cy - EYE_H * 0.5f;
    float y_out = top + LID_OUT_OPEN;
    float y_in  = top + LID_IN_OPEN;
    // 统一成"从左到右"再按镜像换端点
    float yl = mirror ? y_in : y_out;
    float yr = mirror ? y_out : y_in;

    for (int x = (int)floorf((float)cx - rw); x <= (int)ceilf((float)cx + rw); x++) {
        float t   = eye_clamp(((float)x + 0.5f - ((float)cx - rw)) / (float)EYE_W, 0.0f, 1.0f);
        float gap = mirror ? eye_lerp(BROW_GAP_IN, BROW_GAP_OUT, t)
                           : eye_lerp(BROW_GAP_OUT, BROW_GAP_IN, t);
        // 眉尾：越靠外角越往上翘，三次方衰减，才像笔锋而不是斜棍
        float u    = mirror ? 1.0f - t : t;          // 0 = 外角
        float tilt = (1.0f - u) * (1.0f - u) * (1.0f - u) * BROW_TILT;
        float lid  = yl + (yr - yl) * t + LID_BULGE * sinf(EYE_PI * t);
        float yb   = lid - gap - tilt;               // 眉底
        float ya   = yb - BROW_THICK;                // 眉顶

        // 两端各 18% 收成笔锋
        float alpha = eye_clamp(eye_min(t, 1.0f - t) / 0.18f, 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;

        for (int y = (int)floorf(ya); y <= (int)floorf(yb); y++) {
            float av = eye_cover(y, ya, yb);
            if (av <= 0.0f) continue;
            eye_put(x, y, (int)(BROW_LEVEL * av * alpha + 0.5f));
        }
    }
}

// ---- 一只眼 ----
// pct: 0 = 全闭（只剩底部细月牙），100 = 常态弯月
static void eye_draw_eye(int cx, int cy, float pct, float lx, float ly, bool mirror)
{
    const float rw = EYE_W * 0.5f;
    const float rh = EYE_H * 0.5f;
    const float q  = 1.0f - eye_clamp(pct, 0.0f, 100.0f) * 0.01f;   // 闭合量

    float y_out = eye_lerp((float)cy - rh + LID_OUT_OPEN, (float)cy + rh - LID_OUT_CLOSE, q);
    float y_in  = eye_lerp((float)cy - rh + LID_IN_OPEN,  (float)cy + rh - LID_IN_CLOSE,  q);
    // 闭得越紧弧越小：不然闭眼时中段的凸起会顶出底边，变成一道黑线
    float bulge = LID_BULGE * (0.3f + 0.7f * (1.0f - q));

    float yl = mirror ? y_in : y_out;
    float yr = mirror ? y_out : y_in;

    // 瞳孔：偏内侧、贴住上睑，顶部被切掉
    float pcx = (float)cx + (mirror ? -6.0f : 6.0f) + lx;
    float tp  = eye_clamp((pcx - ((float)cx - rw)) / (float)EYE_W, 0.0f, 1.0f);
    float pcy = yl + (yr - yl) * tp + bulge * sinf(EYE_PI * tp) + 5.0f + ly;

    float px0 = pcx - EYE_R * 0.5f, px1 = pcx + EYE_R * 0.5f;
    float py0 = pcy - EYE_R * 0.5f, py1 = pcy + EYE_R * 0.5f;
    // 高光压在瞳孔中心偏上，不能放到瞳孔顶部 —— 那里在睑线以上，会被裁掉
    float hx0 = px0 + 3.0f, hx1 = hx0 + EYE_HI;
    float hy0 = pcy - 5.0f, hy1 = hy0 + EYE_HI;

    for (int x = (int)floorf((float)cx - rw) - 1; x <= (int)ceilf((float)cx + rw) + 1; x++) {
        float dx = ((float)x + 0.5f - (float)cx) / rw;
        if (dx <= -1.0f || dx >= 1.0f) continue;

        float half_h = rh * sqrtf(1.0f - dx * dx);   // 该列的椭圆半高
        float top = (float)cy - half_h;
        float bot = (float)cy + half_h;

        // 上睑线在该列的高度，取和椭圆边界的较下者
        float t   = eye_clamp(((float)x + 0.5f - ((float)cx - rw)) / (float)EYE_W, 0.0f, 1.0f);
        float lid = yl + (yr - yl) * t + bulge * sinf(EYE_PI * t);
        if (lid > top) top = lid;
        if (bot <= top) continue;

        // 底部灰带（眼球厚度）。可见高度太小的时候不画，否则闭眼那条缝会
        // 整条变成灰的，看着像屏幕坏了。
        float vis_h     = bot - top;
        float grey_h    = vis_h > 14.0f ? eye_min(vis_h * 0.22f, 8.0f) : 0.0f;
        float grey_from = bot - grey_h;

        for (int y = (int)floorf(top); y <= (int)floorf(bot); y++) {
            float a = eye_cover(y, top, bot);
            if (a <= 0.0f) continue;

            float base = ((float)y + 0.5f) > grey_from ? 198.0f : 255.0f;
            float v    = base * a;                        // 黑底上叠白

            // 瞳孔：同样被眼白区域裁剪，所以闭眼时它自然消失
            float ax = eye_min((float)x + 1.0f, px1) - eye_max((float)x, px0);
            float ay = eye_min((float)y + 1.0f, py1) - eye_max((float)y, py0);
            if (ax > 0.0f && ay > 0.0f) {
                float ap = eye_min(ax, 1.0f) * eye_min(ay, 1.0f) * a;
                v += (30.0f - v) * ap;
            }

            float bx = eye_min((float)x + 1.0f, hx1) - eye_max((float)x, hx0);
            float by = eye_min((float)y + 1.0f, hy1) - eye_max((float)y, hy0);
            if (bx > 0.0f && by > 0.0f) {
                float ah = eye_min(bx, 1.0f) * eye_min(by, 1.0f) * a;
                v += (255.0f - v) * ah;
            }

            eye_put(x, y, (int)(v + 0.5f));
        }
    }
}

// 整体呼吸：周期 4.2 秒、振幅 1.2px，慢到几乎察觉不到，但停掉会觉得僵
static int eye_breathe(uint32_t ms)
{
    return (int)lroundf(1.2f * sinf((float)ms / 4200.0f * 2.0f * EYE_PI));
}

static void eyes_render(void)
{
    if (!s_eye_buf) return;

    memset(s_eye_buf, 0, (size_t)EYES_BW * EYES_BH * 2);

    int cy = (EYE_CY - EYES_BY) + eye_breathe(s_eyes.ms);
    int xl = EYE_CX_L - EYES_BX;
    int xr = EYE_CX_R - EYES_BX;

    // 顺序不能反：先眉后眼，眼白才会覆盖掉可能重叠的眉毛像素
    eye_draw_brow(xl, cy, false);
    eye_draw_brow(xr, cy, true);
    eye_draw_eye(xl, cy, s_eyes.open_pct, s_eyes.look_x, s_eyes.look_y, false);
    eye_draw_eye(xr, cy, s_eyes.open_pct, s_eyes.look_x, s_eyes.look_y, true);

    lv_obj_invalidate(s_cv_eyes);
}

static uint32_t eyes_blink_interval(void)
{
    // 困 倦时眨眼间隔拉到 7-12 秒，平时 2.4-5.2 秒
    return s_eyes.doze ? (7000 + esp_random() % 5000)
                       : (2400 + esp_random() % 2800);
}

static void eyes_reset(void)
{
    s_eyes.open_pct   = 0.0f;      // 从合着的状态睁眼，像被叫醒
    s_eyes.base_pct   = 100.0f;
    s_eyes.phase_from = 0.0f;
    s_eyes.phase      = EYES_OPENING;
    s_eyes.phase_ms   = 0;
    s_eyes.look_x = 2.0f;   s_eyes.look_y = -1.0f;
    s_eyes.look_tx = 2.0f;  s_eyes.look_ty = -1.0f;
    s_eyes.look_in_ms  = 1400;
    s_eyes.blink_in_ms = 2400;
    s_eyes.doze = false;
    s_eyes.awake_ms    = 0;
    s_eyes.nod_in_ms   = 45000;
    s_eyes.nod_left_ms = 0;
    s_eyes.ms          = 0;
    // 置成不可能的值，保证第一帧一定重画
    s_eyes.last_open    = -999.0f;
    s_eyes.last_lx      = -999.0f;
    s_eyes.last_ly      = -999.0f;
    s_eyes.last_breathe = -999;
}

static void eyes_tick_cb(lv_timer_t *t)
{
    (void)t;
    const uint32_t dt = EYES_MS;
    s_eyes.ms += dt;

    // ---- 犯困：待机满 3 分钟，睑线降到 44%，偶尔点一下头 ----
    if (!s_eyes.doze) {
        s_eyes.awake_ms += dt;
        if (s_eyes.awake_ms >= DOZE_AFTER_MS) {
            s_eyes.doze = true;
            s_eyes.base_pct = 44.0f;
            s_eyes.nod_in_ms = 45000;
            ESP_LOGI(TAG, ">>> 眼睛：开始犯困（睑线 44%%）");
        }
    }
    if (s_eyes.nod_left_ms > 0) {
        s_eyes.nod_left_ms = dt < s_eyes.nod_left_ms ? s_eyes.nod_left_ms - dt : 0;
    } else if (s_eyes.doze) {
        s_eyes.nod_in_ms = dt < s_eyes.nod_in_ms ? s_eyes.nod_in_ms - dt : 0;
        if (s_eyes.nod_in_ms == 0) {
            s_eyes.nod_in_ms   = 40000 + esp_random() % 15000;
            s_eyes.nod_left_ms = 900;
        }
    }

    // ---- 眨眼状态机 ----
    switch (s_eyes.phase) {
    case EYES_IDLE:
        s_eyes.blink_in_ms = dt < s_eyes.blink_in_ms ? s_eyes.blink_in_ms - dt : 0;
        if (s_eyes.blink_in_ms == 0) {
            s_eyes.phase      = EYES_CLOSING;
            s_eyes.phase_ms   = 0;
            s_eyes.phase_from = s_eyes.open_pct;
        }
        break;

    case EYES_CLOSING: {
        s_eyes.phase_ms += dt;
        float p = eye_min((float)s_eyes.phase_ms / EYE_CLOSE_MS, 1.0f);
        s_eyes.open_pct = eye_lerp(s_eyes.phase_from, 0.0f, p * p);   // ease-in
        if (p >= 1.0f) { s_eyes.phase = EYES_HOLD; s_eyes.phase_ms = 0; }
        break;
    }

    case EYES_HOLD:
        s_eyes.phase_ms += dt;
        s_eyes.open_pct = 0.0f;
        if (s_eyes.phase_ms >= EYE_HOLD_MS) {
            s_eyes.phase = EYES_OPENING;
            s_eyes.phase_ms = 0;
        }
        break;

    default: {   // EYES_OPENING
        s_eyes.phase_ms += dt;
        float p = eye_min((float)s_eyes.phase_ms / EYE_OPEN_MS, 1.0f);
        s_eyes.open_pct = eye_lerp(0.0f, s_eyes.base_pct, 1.0f - (1.0f - p) * (1.0f - p));
        if (p >= 1.0f) {
            s_eyes.phase    = EYES_IDLE;
            s_eyes.phase_ms = 0;
            // 12% 概率紧接着再眨一下（连眨），比每次都等满间隔更像活物
            s_eyes.blink_in_ms = (esp_random() % 100 < 12) ? 160 : eyes_blink_interval();
        }
        break;
    }
    }

    // 空闲态向"目标开合"缓动。眨眼状态机在跑的时候由它自己说了算。
    if (s_eyes.phase == EYES_IDLE) {
        float target;
        if (s_eyes.nod_left_ms > 0)   target = eye_min(s_eyes.open_pct, 10.0f);
        else if (s_eyes.doze)         target = s_eyes.base_pct;
        else                          target = 100.0f;
        float k = eye_min((float)dt / 260.0f, 1.0f);
        s_eyes.open_pct += (target - s_eyes.open_pct) * k;
    }

    // ---- 视线漂移 ----
    s_eyes.look_in_ms = dt < s_eyes.look_in_ms ? s_eyes.look_in_ms - dt : 0;
    if (s_eyes.look_in_ms == 0) {
        static const int8_t k_pick[8][2] = {
            {0, 0}, {0, 0}, {7, 0}, {-7, 0}, {0, -4}, {0, 2}, {5, -3}, {-5, 2}
        };
        s_eyes.look_in_ms = s_eyes.doze ? (3000 + esp_random() % 4000)
                                        : (1400 + esp_random() % 2200);
        int i = (int)(esp_random() % 8);
        float scale = s_eyes.doze ? 0.45f : 1.0f;   // 困了就不怎么东张西望
        s_eyes.look_tx = k_pick[i][0] * scale;
        s_eyes.look_ty = k_pick[i][1] * scale;
    }
    float lk = eye_min((float)dt / 120.0f, 1.0f);
    s_eyes.look_x += (s_eyes.look_tx - s_eyes.look_x) * lk;
    s_eyes.look_y += (s_eyes.look_ty - s_eyes.look_y) * lk;

    // ---- 只有真变了才重画 ----
    int breathe = eye_breathe(s_eyes.ms);
    bool changed = fabsf(s_eyes.open_pct - s_eyes.last_open) > 0.8f
                || fabsf(s_eyes.look_x   - s_eyes.last_lx)   > 0.4f
                || fabsf(s_eyes.look_y   - s_eyes.last_ly)   > 0.4f
                || breathe != s_eyes.last_breathe;
    if (!changed) return;

    s_eyes.last_open    = s_eyes.open_pct;
    s_eyes.last_lx      = s_eyes.look_x;
    s_eyes.last_ly      = s_eyes.look_y;
    s_eyes.last_breathe = breathe;
    eyes_render();
}

static void eyes_build(lv_obj_t *parent)
{
    const size_t bytes = (size_t)EYES_BW * EYES_BH * 2;

    s_eye_buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_eye_buf) {
        // 没 PSRAM 就退化成纯时钟屏。为了眼睛把整机拖垮不值得。
        ESP_LOGE(TAG, "眼睛画布分配失败（%u 字节），休眠屏退化为纯时钟",
                 (unsigned)bytes);
        return;
    }
    memset(s_eye_buf, 0, bytes);

    s_cv_eyes = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_cv_eyes, s_eye_buf, EYES_BW, EYES_BH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_cv_eyes, EYES_BX, EYES_BY);

    s_eye_timer = lv_timer_create(eyes_tick_cb, EYES_MS, NULL);
    lv_timer_pause(s_eye_timer);        // 只在休眠时跑，醒着时一帧都不算

    ESP_LOGI(TAG, "眼睛画布: %dx%d RGB565, %u KB (PSRAM)",
             EYES_BW, EYES_BH, (unsigned)(bytes / 1024));
}

// 眼睛模块的对外钩子：只有休眠屏进/出时用
static void eyes_enter(void)
{
    if (!s_eye_timer) return;
    eyes_reset();
    lv_timer_resume(s_eye_timer);
}

static void eyes_leave(void)
{
    if (s_eye_timer) lv_timer_pause(s_eye_timer);
}

// ---- 休眠屏 ----
// 空转时屏幕上只剩"两只眼睛 + 几点 + 还在线"，其余全藏掉。
//
// 布局（240x240）：
//        ┌──────────────────┐
//        │  ╱╲        ╱╲    │  ← 眉毛 y=27..44
//        │ ◡  ◡      ◡  ◡   │  ← 眼白 y=39..93（画布覆盖 22..104）
//        │      14:43       │  ← 28px，行高 35，y=134..169
//        │      ● 在线      │  ← 12px，y=197
//        └──────────────────┘
//
// 时间从 72px 缩到 28px，是为了给眼睛让位 —— 眼睛是主角，时间退成配角。
// 代价是半米外看不清时间，得凑近、或者按键唤醒看页头左上角那个时钟
// （主界面一直有 28px 时钟，信息没丢）。
static void sleep_ui_build(void)
{
    lv_obj_t *scr = lv_screen_active();

    // 容器全屏不透明：底下那些控件虽然被 HIDDEN 了，但休眠屏是叠在
    // scr 上的，用透明底会漏出屏幕底色之外的东西（比如残影）。
    // 铺一层纯黑最稳，也和正常界面同色，切换时不闪。
    s_pg_sleep = mk_page(scr);
    lv_obj_set_style_bg_color(s_pg_sleep, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_pg_sleep, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_pg_sleep, LV_OBJ_FLAG_HIDDEN);

    // 眼睛：逐像素画布，盖住 y=22..104 这一条。
    // 必须建在 sleep_ui_build 里 —— 它是 s_pg_sleep 的子对象，
    // 跟着容器一起显示/隐藏，不需要单独管可见性。
    eyes_build(s_pg_sleep);

    // 时间。用 TOP_MID + 固定 y：label 高度就是 font line_height(35)，
    // 数字墨迹落在盒内偏下（base_line=7），所以 y 取 134 让墨迹
    // 落在 142..162，视觉上离眼睛底部(93)和状态点(197)都不挤。
    s_lbl_bigclock = mk_label(s_pg_sleep, &font_cn_28, COL_TEXT,
                              LV_ALIGN_TOP_MID, 0, 134);
    lv_label_set_text(s_lbl_bigclock, "--:--");

    // 在线状态点。只在休眠屏出现，和页头那个"● 在线"是两份独立控件 ——
    // 复用同一个标签就得在休眠/唤醒时改名、改字体、改坐标，反而更容易错。
    // 一个 12px 标签的 RAM 代价可以忽略。
    s_lbl_sleep_dot = mk_label(s_pg_sleep, &font_cn_12, COL_OK,
                               LV_ALIGN_TOP_MID, 0, 197);
    lv_label_set_text(s_lbl_sleep_dot, "● 在线");
}

void ui_board_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- logo + 时钟 ----
    // logo 24x24 RGB565，透明底已预合成到 COL_BG，由 tools/gen_logo.py 生成。
    // 先摆 logo 再让时钟参照它对齐，比手算像素可靠：
    // OUT_RIGHT_MID 保证两者垂直中心线重合，改 logo 尺寸时时钟会自动跟着移。
    //
    // y 从 0 改成 4：原来 logo 顶边贴 y=0，而时钟 30px 行高以 logo 中心(12)
    // 对齐后顶边落到 y=-3，被屏幕顶端裁掉 3px，整体观感偏上。
    // 下移 4px 后 logo 占 4..28、时钟占 1..31，顶边不越界，底部离分隔线(36)还有 5px。
    LV_IMAGE_DECLARE(logo_wb);
    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &logo_wb);
    lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 8, HEADER_LOGO_Y);

    s_lbl_clock = mk_label(scr, &font_cn_28, COL_TEXT, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_clock, "--:--");
    lv_obj_align_to(s_lbl_clock, logo, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    s_lbl_date = mk_label(scr, &font_cn_12, COL_DIM, LV_ALIGN_TOP_RIGHT, BAT_TEXT_RIGHT, 6);
    lv_label_set_text(s_lbl_date, "--");

    s_lbl_status = mk_label(scr, &font_cn_12, COL_DIM, LV_ALIGN_TOP_RIGHT, BAT_TEXT_RIGHT, 20);
    lv_label_set_text(s_lbl_status, "● 连接中");

    // ---- 电量圆圈（右上角）----
    // lv_arc 的角度体系有两个必踩的坑，这里都绕开了：
    //
    // 坑1：set_bg_angles 内部会把 >360 的值 `while(x>360) x-=360` 折回来。
    //      所以 set_bg_angles(90, 450) 会变成 (90, 90) —— 360° 的整圈
    //      退化成 0°，圆圈整个不可见。整圈只能用 (0,360) 这个特例，
    //      起始方向改用 set_rotation 旋转来实现。
    //
    // 坑2：0° 在三点钟方向、顺时针增长。想让进度从 12 点开始，
    //      需要 rotation = 270（因为绘制时用的是 angle + rotation）。
    s_arc_bat = lv_arc_create(scr);
    lv_obj_set_size(s_arc_bat, BAT_ARC_SIZE, BAT_ARC_SIZE);
    lv_obj_align(s_arc_bat, LV_ALIGN_TOP_RIGHT, BAT_ARC_X, BAT_ARC_Y);
    lv_arc_set_bg_angles(s_arc_bat, 0, 360);    // 整圈背景轨道
    lv_arc_set_rotation(s_arc_bat, 270);        // 起点转到 12 点
    lv_arc_set_range(s_arc_bat, 0, 100);
    lv_arc_set_value(s_arc_bat, 0);
    // 去掉旋钮和点击交互 —— 这是指示器不是滑块，能被拖动会误触
    lv_obj_remove_style(s_arc_bat, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_bat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc_bat, BAT_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_bat, BAT_ARC_WIDTH, LV_PART_INDICATOR);
    // 背景轨道：暗灰，让"未充满"的部分也能看出是圆圈
    lv_obj_set_style_arc_color(s_arc_bat, COL_DIM, LV_PART_MAIN);
    // 未填充的轨道。底色变深后 30% 太弱，看不出圆圈轮廓，提到 OPA_50
    lv_obj_set_style_arc_opa(s_arc_bat, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_bat, COL_OK, LV_PART_INDICATOR);
    // 方头，和图表环一致。rounded=true 会把指示弧**两端各延长 width/2**
    // （3px/2 = 1.5px）再切圆，两端共多 3px。这个小圈周长只有 66.0px
    // （24px 环、弧宽 3px → 中线圈半径 10.5px，2πr = 65.97px），
    // 1% 只有 0.66px，所以圆头带来的视觉弧长恒多 **4.55 个百分点**
    // ——50% 看着像 55%。100% 时两个半圆头还会在 12 点撞在一起。
    // （34px 那版周长 97.4px，误差是 3.1 个百分点；缩小后误差反而变大，
    //   因为圆头的绝对长度不变、周长却短了近三分之一 —— 方头更不可省。）
    lv_obj_set_style_arc_rounded(s_arc_bat, false, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc_bat, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_arc_bat, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_arc_bat, 0, LV_PART_MAIN);

    // 圆心：只有充电时显示一个 "+"，放电时为空。
    //
    // 为什么彻底不给百分比数字：
    //   24px 圆环的内径只有 18px，font_cn_12 一个字符 6px，
    //   "100%" 要 18px —— 正好等于内径，一个字都挤不下，两边会压到弧上。
    //   既然数字放不进去，那就不放了：弧长本身就是"多少"，
    //   要精确数字可以按键切到系统屏（那里有完整指标）。
    //   这也解决了原来"100% 略微超出内径"的将就。
    //
    // 定位：定尺寸文本框 + 文本居中 + 与圆圈同心（不用 align_to，那是一次性计算）。
    // 宽度留得比圆环大一点没关系，因为只有单个 "+"，居中后就在圆心。
    s_lbl_chrg = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_chrg, &font_cn_12, 0);
    lv_obj_set_style_text_color(s_lbl_chrg, COL_OK, 0);
    lv_obj_set_style_text_align(s_lbl_chrg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(s_lbl_chrg, 0, 0);
    lv_obj_set_size(s_lbl_chrg, BAT_ARC_SIZE, BAT_CHRG_BOX_H);
    lv_obj_align(s_lbl_chrg, LV_ALIGN_TOP_RIGHT, BAT_ARC_X, BAT_ARC_Y + BAT_CHRG_DY);
    lv_label_set_text(s_lbl_chrg, "");

    // ---- WorkBuddy logo 已移到时钟左侧，见上方"logo + 时钟" ----

    mk_hline(scr, 36);

    // ---- 两组屏容器（全屏透明，不拦截任何东西）----
    s_pg_main = mk_page(scr);
    s_pg_list = mk_page(scr);
    lv_obj_add_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);   // 开机停在概览屏

    // ---- 三格统计（每格 72 宽 + 4 间距，共 224）----
    static const char *captions[3] = { "⚙ 工作", "✓ 完成", "⚡ 自动" };
    static const lv_color_t colors[3] = { COL_ACCENT, COL_OK, COL_WARN };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *box = lv_obj_create(s_pg_main);
        lv_obj_set_size(box, 72, 46);
        lv_obj_set_pos(box, 8 + i * 76, 42);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x161B22), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        s_lbl_stat[i] = lv_label_create(box);
        lv_obj_set_style_text_font(s_lbl_stat[i], &font_cn_28, 0);
        lv_obj_set_style_text_color(s_lbl_stat[i], colors[i], 0);
        lv_label_set_text(s_lbl_stat[i], "0");
        lv_obj_align(s_lbl_stat[i], LV_ALIGN_TOP_MID, 0, -4);

        lv_obj_t *cap = lv_label_create(box);
        lv_obj_set_style_text_font(cap, &font_cn_12, 0);
        lv_obj_set_style_text_color(cap, COL_DIM, 0);
        lv_label_set_text(cap, captions[i]);
        lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, 1);
    }

    // ---- 会话列表（3 条 x 40px）----
    // 行首 6px 状态灯 + 标题跑马灯。标题右侧到 232，所以宽度 214。
    for (int i = 0; i < MAX_SESSIONS; i++) {
        lv_coord_t y = 94 + i * 40;

        s_dot[i] = lv_obj_create(s_pg_main);
        lv_obj_set_size(s_dot[i], 6, 6);
        lv_obj_set_pos(s_dot[i], 8, y + 7);
        lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(0x2A3038), 0);
        lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_dot[i], 0, 0);
        lv_obj_set_style_pad_all(s_dot[i], 0, 0);
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);

        s_lbl_sess_title[i] = mk_label(s_pg_main, &font_cn_16, COL_TEXT,
                                       LV_ALIGN_TOP_LEFT, 18, y);
        lv_label_set_text(s_lbl_sess_title[i], "");
        lv_obj_set_width(s_lbl_sess_title[i], 214);
        // 一行显示，放不下就横向循环滚动（放得下时 LVGL 自动不动）
        lv_label_set_long_mode(s_lbl_sess_title[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        // 滚动一轮的时长，太慢看不完、太快读不清，6s 折中
        lv_obj_set_style_anim_duration(s_lbl_sess_title[i], 6000, 0);

        s_lbl_sess_meta[i] = mk_label(s_pg_main, &font_cn_12, COL_DIM,
                                      LV_ALIGN_TOP_LEFT, 18, y + 19);
        lv_label_set_text(s_lbl_sess_meta[i], "");
        lv_obj_set_width(s_lbl_sess_meta[i], 214);
        lv_label_set_long_mode(s_lbl_sess_meta[i], LV_LABEL_LONG_DOT);

        lv_obj_add_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ---- 详情屏：页标题 + 最多 6 行（每行 23px，末行到 210，不压页脚线 216）----
    s_lbl_page = mk_label(s_pg_list, &font_cn_16, COL_ACCENT,
                          LV_ALIGN_TOP_LEFT, 8, 42);
    lv_label_set_text(s_lbl_page, "");
    lv_obj_set_width(s_lbl_page, 200);
    lv_label_set_long_mode(s_lbl_page, LV_LABEL_LONG_DOT);

    mk_hline(s_pg_list, 66);

    for (int i = 0; i < MAX_ROWS; i++) {
        lv_coord_t y = 72 + i * 23;

        // 左：16px 主文本，宽度让出右侧数值的位置
        s_lbl_row_t[i] = mk_label(s_pg_list, &font_cn_16, COL_TEXT,
                                  LV_ALIGN_TOP_LEFT, 8, y + 2);
        lv_label_set_text(s_lbl_row_t[i], "");
        lv_obj_set_width(s_lbl_row_t[i], 170);
        lv_label_set_long_mode(s_lbl_row_t[i], LV_LABEL_LONG_DOT);

        // 右：12px 数值
        s_lbl_row_v[i] = mk_label(s_pg_list, &font_cn_12, COL_DIM,
                                  LV_ALIGN_TOP_RIGHT, -8, y + 5);
        lv_label_set_text(s_lbl_row_v[i], "");

        lv_obj_add_flag(s_lbl_row_t[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_row_v[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ---- 图表屏：三个环形仪表 ----
    // 只在服务端给 kind="chart" 时显示。控件常驻、切页只改可见性，
    // 和列表屏同一套路 —— 每次切屏重建控件会闪白。
    s_pg_chart = mk_page(scr);
    lv_obj_add_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);

    s_lbl_chart_title = mk_label(s_pg_chart, &font_cn_16, COL_ACCENT,
                                 LV_ALIGN_TOP_LEFT, 8, 42);
    lv_label_set_text(s_lbl_chart_title, "");
    lv_obj_set_width(s_lbl_chart_title, 200);
    lv_label_set_long_mode(s_lbl_chart_title, LV_LABEL_LONG_DOT);
    mk_hline(s_pg_chart, 66);

    for (int i = 0; i < MAX_CHARTS; i++) {
        lv_obj_t *arc = lv_arc_create(s_pg_chart);
        lv_obj_set_size(arc, CHART_ARC_SIZE, CHART_ARC_SIZE);
        lv_obj_set_pos(arc, CHART_ARC_X0 + i * CHART_ARC_STEP, CHART_ARC_Y);
        // 和电量弧踩过的是同一组坑：整圈只能 (0,360)，起点靠 rotation 转到 12 点
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_arc_set_rotation(arc, 270);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 0);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(arc, CHART_ARC_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, CHART_ARC_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, COL_DIM, LV_PART_MAIN);
        // 底色是纯黑，OPA_50 的灰轨刚好能看出"未填满的半圈"
        lv_obj_set_style_arc_opa(arc, LV_OPA_50, LV_PART_MAIN);
        // 方头，不能用圆头（rounded=true）。
        //
        // LVGL 的 rounded 不是"把端点切圆"，而是把指示弧的**两端各延长
        // 线宽的一半**（7px/2 = 3.5px）再切圆 —— 用来做圆头针脚。
        // 副作用是弧的起点从 12 点前移到 11 点方向，终点也多出一截。
        // 实测 86% 的磁盘环上，12 点处出现黄蓝交错的锯齿带，很难看。
        // 方头则严格从 12 点画起，起点位置和百分比严格对应。
        lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
        s_arc_chart[i] = arc;

        // 环内百分比。16px 字体下 "100%" 行框 38px，内径 48px 装得下。
        //
        // 定位方式见 CHART_VALUE_BOX_* 常量的注释：不用 align_to，
        // 用"和环同宽同高的定尺寸文本框 + 文本居中"，位置直接算坐标。
        // 文本框与环同尺寸同位置 → 框心 == 环心 → 文字在框内居中即在环心。
        s_lbl_chart_v[i] = lv_label_create(s_pg_chart);
        lv_obj_set_style_text_font(s_lbl_chart_v[i], &font_cn_16, 0);
        lv_obj_set_style_text_color(s_lbl_chart_v[i], COL_TEXT, 0);
        lv_obj_set_style_text_align(s_lbl_chart_v[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(s_lbl_chart_v[i], 0, 0);
        lv_obj_set_size(s_lbl_chart_v[i], CHART_VALUE_BOX_W, CHART_VALUE_BOX_H);
        lv_obj_set_pos(s_lbl_chart_v[i],
                       CHART_ARC_X0 + i * CHART_ARC_STEP
                           + (CHART_ARC_SIZE - CHART_VALUE_BOX_W) / 2,
                       CHART_ARC_Y
                           + (CHART_ARC_SIZE - CHART_VALUE_BOX_H) / 2);
        lv_label_set_text(s_lbl_chart_v[i], "--");

        // 环下名称，宽度跟环一致 + 居中，否则"内存"两个字会偏左
        s_lbl_chart_k[i] = mk_label(s_pg_chart, &font_cn_12, COL_DIM,
                                    LV_ALIGN_TOP_LEFT,
                                    CHART_ARC_X0 + i * CHART_ARC_STEP,
                                    CHART_LABEL_Y);
        lv_label_set_text(s_lbl_chart_k[i], "");
        lv_obj_set_width(s_lbl_chart_k[i], CHART_ARC_SIZE);
        lv_obj_set_style_text_align(s_lbl_chart_k[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    s_lbl_chart_l = mk_label(s_pg_chart, &font_cn_12, COL_DIM,
                             LV_ALIGN_TOP_LEFT, 8, CHART_FOOT_Y);
    lv_label_set_text(s_lbl_chart_l, "");
    s_lbl_chart_r = mk_label(s_pg_chart, &font_cn_12, COL_DIM,
                             LV_ALIGN_TOP_RIGHT, -8, CHART_FOOT_Y);
    lv_label_set_text(s_lbl_chart_r, "");

    // 黄灯闪烁：500ms 一个相位，只动透明度不动颜色（颜色变化会触发重排）
    lv_timer_create(blink_timer_cb, 500, NULL);

    // ---- 页脚：左设备指标，右最后更新时间 ----
    mk_hline(scr, 216);
    s_lbl_sys = mk_label(scr, &font_cn_12, COL_DIM, LV_ALIGN_TOP_LEFT, 8, 222);
    lv_label_set_text(s_lbl_sys, "等待数据");
    s_lbl_footer = mk_label(scr, &font_cn_12, COL_DIM,
                            LV_ALIGN_TOP_RIGHT, FOOTER_TEXT_RIGHT, 222);
    lv_label_set_text(s_lbl_footer, "");

    // ---- 像素宠物：右下角浮动，占屏幕一半 ----
    // 建图、半透明、最底层、换帧定时器都在 pet_view.cpp。
    pet_view_build(scr);

    // ---- 按键提示（默认隐藏，只在按的时候闪一下）----
    // 原来放 BOTTOM_RIGHT(-8,-4)，正好被页脚宠物整块盖住 —— 挪到分隔线上方居中。
    // 只在按住键的那一秒多出现，短暂压住会话行可以接受。
    s_lbl_toast = mk_label(scr, &font_cn_16, COL_ACCENT,
                           LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_add_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);

    // ---- 休眠屏（空转时只显示时间和在线状态）----
    // 建在 WiFi/服务浮层之前、宠物之后，这样两层浮层仍然压在它上面
    // （连不上网的时候该报警报，不该被一块时钟挡住）。
    //
    // 整屏只有一个容器，休眠时把其余的页头/页脚/宠物/页面容器全部藏掉，
    // 只留这一个 —— 比逐个控件改可见性可靠，退出时也只需要反向操作一次。
    sleep_ui_build();

    // ---- WiFi 状态浮层 ----
    // 只有"连不上"和"配网中"两个状态才显示，盖住整屏（含宠物和页脚）。
    // 放在最后创建 = 在子对象顺序里最靠上，天然盖住所有内容，不用管 z 序。
    s_pg_wifi = mk_page(scr);
    // mk_page 的底色是透明的，这里要一块不透明底，否则下面那屏的字会透上来
    lv_obj_set_style_bg_color(s_pg_wifi, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_pg_wifi, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_pg_wifi, LV_OBJ_FLAG_HIDDEN);

    s_lbl_wifi_title = mk_label(s_pg_wifi, &font_cn_28, COL_WARN,
                                LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(s_lbl_wifi_title, "WiFi 未连接");

    s_lbl_wifi_l1 = mk_label(s_pg_wifi, &font_cn_16, COL_TEXT,
                             LV_ALIGN_TOP_MID, 0, 100);
    lv_label_set_text(s_lbl_wifi_l1, "");

    s_lbl_wifi_l2 = mk_label(s_pg_wifi, &font_cn_16, COL_ACCENT,
                             LV_ALIGN_TOP_MID, 0, 132);
    lv_label_set_text(s_lbl_wifi_l2, "");

    s_lbl_wifi_l3 = mk_label(s_pg_wifi, &font_cn_12, COL_DIM,
                             LV_ALIGN_TOP_MID, 0, 178);
    lv_label_set_text(s_lbl_wifi_l3, "");
    // 长按提示可能超过 224px，允许换行
    lv_label_set_long_mode(s_lbl_wifi_l3, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_wifi_l3, 216);
    lv_obj_set_style_text_align(s_lbl_wifi_l3, LV_TEXT_ALIGN_CENTER, 0);

    // 服务地址浮层：建在最后，子对象顺序最靠上
    svr_ui_build();

    // 亮度提示挪到最上层。它是"系统级"的瞬时反馈，任何界面都该能看见：
    //   - 建屏时它排在第 1295 行，而休眠屏容器在第 1305 行才建 ——
    //     后建的在子对象顺序里更靠上，休眠时按 VOL 调亮度会发现
    //     屏幕确实变亮了，但"☀ 70%"这几个字被休眠屏的纯黑底盖掉了。
    //   - 同理也不能被 WiFi / 服务地址浮层压住：那两层在连不上网时
    //     会长期占屏，而用户这时恰恰更需要看到亮度反馈。
    // 把 toast 移到最后一个子对象即可（LVGL 没有 z-index，顺序即层级）。
    // 这个操作放在 ui_board_build 末尾，此时所有控件都已创建完毕。
    if (s_lbl_toast) lv_obj_move_foreground(s_lbl_toast);
}

// ===========================================================================
// 渲染
//
// ⚠ 这组函数**不持数据快照锁**，只读传入的快照。
// 调用方（lvgl_task）负责：持锁 memcpy 出快照 → 放锁 → 再传进来绘制。
// 绘制要走 SPI，耗时可达几十毫秒；锁内绘制会堵住 fetch 任务，
// 导致那一轮数据被静默丢弃。
// ===========================================================================

// 服务端只发 0..4 的小整数，色板留在固件 —— 换配色不用改服务端
static lv_color_t row_color(uint8_t c)
{
    switch (c) {
        case 1:  return COL_ACCENT;
        case 2:  return COL_OK;
        case 3:  return COL_WARN;
        case 4:  return COL_DIM;
        default: return COL_TEXT;
    }
}

// 图表色板。和 row_color 的区别只有一个：扩了 5 = 红。
// 文本行的 4 是灰（"无数据"），图表要的是"正常色 → 危险色"两档，
// 灰色放进度条上等于没填，所以单独开一个码，不去动 row_color 的既有含义。
static lv_color_t chart_color(uint8_t c)
{
    switch (c) {
        case 1:  return COL_ACCENT;
        case 2:  return COL_OK;
        case 3:  return COL_WARN;
        case 5:  return COL_ERR;
        default: return COL_DIM;
    }
}

// 第 0 屏主体：三格统计 + 会话列表

// 秒数 → "12s" / "7m23s" / "2h05m" / "3d4h"
//
// 区别于服务端 human_ago() 只给 `7m`：那种粒度一秒内不变，想"按秒走动"就
// 必须多带一位。分档口径和 human_ago 对齐，只是每档补上更细的单位。
// 最长的 "59m59s" 是 6 个 ASCII 字符，12px 字体约 42px，左侧 214px 装得下。
static void fmt_age(char *out, size_t n, int sec)
{
    if (sec < 0) sec = 0;
    if (sec < 60) {
        snprintf(out, n, "%ds", sec);
    } else if (sec < 3600) {
        snprintf(out, n, "%dm%02ds", sec / 60, sec % 60);
    } else if (sec < 86400) {
        snprintf(out, n, "%dh%02dm", sec / 3600, (sec % 3600) / 60);
    } else {
        snprintf(out, n, "%dd%dh", sec / 86400, (sec % 86400) / 3600);
    }
}

// 会话此刻的年龄（秒）= 服务端拍下的值 + 本地从那一刻起流逝的时间。
// 读的是快照，不加锁。
static int session_age_s(const board_data_t *d, int idx)
{
    int64_t extra = (esp_timer_get_time() / 1000) - d->sample_ms;
    if (extra < 0) extra = 0;            // 时钟异常时不要倒退
    return d->sessions[idx].age_s + (int)(extra / 1000);
}

// 页脚右侧的"数据新鲜度"：3s前 / 12m前 / 2d前
//
// 为什么不显示 clock（原来的 "★ 09:44"）：
//   那是**服务端此刻的钟点**，每轮轮询都在变，和数据是否新鲜毫无关系 ——
//   断网十分钟后它照样跳到当前时刻，看着像刚更新过。真正有意义的是
//   快照有多旧，服务端在 payload 里给了 age（秒）。
//
// 为什么要本地再累加：
//   age 是服务端拍下那一刻的值，只随 5 秒轮询刷新。直接用它会"5 秒一跳"，
//   看着像卡住。锚点用 d->sample_ms（解析这批数据的本地时刻，会话计时
//   也用它），屏幕上就是每秒一跳的真时间。
//   注意：真断网（ok=false）时渲染走离线分支，页脚右侧会被清空、
//   左侧让给错误原因，所以这条只在在线时可见。"断了多久"由页首的
//   "● 离线"承担，不靠这里显示。
//
// 为什么不带"更新"两个字：
//   12px 字体下 "更新 59m前" 要 61px，而页脚左侧
//   "CPU 100%  MEM 100%  DSK 100%" 最坏要 181px。左边从 x=8 起、
//   右边距右缘 8px，两者之和不能超过 224px，带前缀就顶到 242px 叠字。
//   去掉前缀后最长 "59m前" 只有 34px，最坏 215px，任何数值组合都不重叠。
static void footer_render_age(const board_data_t *d)
{
    char buf[24];
    if (d->age < 0) {
        // 旧版服务端没下发 age：明确显示 --，不假装"刚刚更新"
        snprintf(buf, sizeof(buf), "--");
    } else {
        int64_t extra = (esp_timer_get_time() / 1000) - d->sample_ms;
        if (extra < 0) extra = 0;        // 时钟异常时不要倒退
        int sec = d->age + (int)(extra / 1000);
        // 分档比 fmt_age 粗：这里只表示"多久没更新"，不需要秒级精度
        if (sec < 60)         snprintf(buf, sizeof(buf), "%ds前", sec);
        else if (sec < 3600)  snprintf(buf, sizeof(buf), "%dm前", sec / 60);
        else if (sec < 86400) snprintf(buf, sizeof(buf), "%dh前", sec / 3600);
        else                  snprintf(buf, sizeof(buf), "%dd前", sec / 86400);
    }
    if (strcmp(buf, s_footer_cache) == 0) return;   // 字符串没变，不重绘
    strncpy(s_footer_cache, buf, sizeof(s_footer_cache) - 1);
    s_footer_cache[sizeof(s_footer_cache) - 1] = '\0';
    lv_label_set_text(s_lbl_footer, buf);
}

static void ui_render_overview(const board_data_t *d)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", d->working);
    lv_label_set_text(s_lbl_stat[0], buf);
    snprintf(buf, sizeof(buf), "%d", d->today_done);
    lv_label_set_text(s_lbl_stat[1], buf);
    snprintf(buf, sizeof(buf), "%d", d->automations);
    lv_label_set_text(s_lbl_stat[2], buf);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (i < d->session_count) {
            char title[48], proj[24];
            /* ⚠ 校验用的字体必须和渲染用的字体一致。
             * 标题渲染用 font_cn_16、项目名渲染用 font_cn_12（进 meta 行），
             * 但校验时两个都用了 font_cn_12。两个字库的字符集不完全一样，
             * 于是会出现"校验说这个字没有 → 换成 ?"，而实际渲染的字体
             * 本来能显示；或者反过来，校验放行但渲染成方框。
             * 校验与渲染分家，症状就是屏上凭空多出问号。 */
            utf8_sanitize(&font_cn_16, d->sessions[i].title,   title, sizeof(title));
            utf8_sanitize(&font_cn_12, d->sessions[i].project, proj,  sizeof(proj));

            // 内容没变就别重设 —— set_text 会把跑马灯位置打回开头
            if (strcmp(title, s_title_cache[i]) != 0) {
                strncpy(s_title_cache[i], title, sizeof(s_title_cache[i]) - 1);
                s_title_cache[i][sizeof(s_title_cache[i]) - 1] = '\0';
                lv_label_set_text(s_lbl_sess_title[i], title);
            }

            // 计时：服务端下发了原始秒数(e)就本地每秒累加，否则回退到它算好的串。
            // 这行每秒都会重设文字，但内容不变时 strcmp 会拦住，不会白白刷屏；
            // 周期性的 set_text 也不影响上面标题的跑马灯（那是另一个控件）。
            char agebuf[TEXT_ELAPSED];
            const char *age;
            if (d->sessions[i].age_s >= 0) {
                fmt_age(agebuf, sizeof(agebuf), session_age_s(d, i));
                age = agebuf;
            } else {
                age = d->sessions[i].elapsed;   // 旧版服务端没发 e
            }

            // 动作文本（"跑命令""等模型""等你回答"……）
            // 放在 meta 行的项目名前面，两者一起被 64 字节截断，
            // 截断后优先保留"动作"（用户最关心的那个信息）。
            char act[16];
            act[0] = '\0';
            if (d->sessions[i].action[0]) {
                utf8_sanitize(&font_cn_12, d->sessions[i].action, act, sizeof(act));
            }

            char meta[64];
            if (act[0]) {
                snprintf(meta, sizeof(meta), "%s · %s", act, age);
            } else {
                snprintf(meta, sizeof(meta), "%s · %s", proj, age);
            }
            // meta 也判等，避免每轮都触发该行的重绘
            if (strcmp(meta, s_meta_cache[i]) != 0) {
                strncpy(s_meta_cache[i], meta, sizeof(s_meta_cache[i]) - 1);
                s_meta_cache[i][sizeof(s_meta_cache[i]) - 1] = '\0';
                lv_label_set_text(s_lbl_sess_meta[i], meta);
            }

            // 等待操作 → 灯变黄 + 交给 blink_timer_cb 闪；不等待就整颗隐藏，
            // 行首留 10px 白，比常亮一个灰点干净
            //
            // 实时状态 st：1=运行中（绿灯常亮），2=等你确认（黄灯闪）。
            // st 优先于 w："等你确认"比"还在干活"更要紧急，
            // 同时命中时取 st=2。旧服务端不发 st，就回退到 w 的旧行为。
            uint8_t st = d->sessions[i].state;
            bool waiting = (st == 2) || (st == 0 && d->sessions[i].waiting != 0);
            bool running = (st == 1) && !waiting;
            s_waiting[i] = waiting;
            s_state[i] = st;
            if (waiting) {
                lv_obj_set_style_bg_color(s_dot[i], COL_WARN, 0);
                lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
                lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            } else if (running) {
                lv_obj_set_style_bg_color(s_dot[i], COL_OK, 0);
                lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
                lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            }

            lv_obj_clear_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            s_waiting[i] = false;
            s_state[i] = 0;
            lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (d->session_count == 0) {
        // 空闲时第 0 行被"接管"成一行的状态提示（"空闲中"），
        // 而不是真的有一条会话。上面那个循环已经把第 0 行藏了，
        // 这里再把它点亮，并顺手清掉 meta 缓存 —— 见下方说明。
        //
        // ⚠ 必须同步写 s_title_cache[0]，否则会出现"有任务却一直显示空闲中"。
        //
        // s_lbl_sess_title[0] 被"空闲中"和"第 0 条会话标题"两个分支共用，
        // 而上面会话分支是用 strcmp(title, s_title_cache[i]) 判等来决定
        // 要不要 set_text 的。如果空闲分支只改 label、不改缓存，就会出现：
        //
        //   t1: 有任务，标题 "检查完成原因"  → 缓存 = "检查完成原因"
        //   t2: 空闲                          → 屏上 "空闲中"，缓存仍是 "检查完成原因"
        //   t3: 又来了同一个任务，标题没变    → strcmp 相等 → **跳过 set_text**
        //
        // 结果就是"明明有任务，屏上却一直写着空闲中"，而且会一直卡着，
        // 直到下一个标题不同的任务才恢复。
        //
        // 不用常量字面量直接比较：写成变量是为了让下面两处 strcmp/strncpy
        // 完全对称，以后加分支时不容易漏掉某一处。
        static const char idle_txt[] = "空闲中";
        if (strcmp(idle_txt, s_title_cache[0]) != 0) {
            strncpy(s_title_cache[0], idle_txt, sizeof(s_title_cache[0]) - 1);
            s_title_cache[0][sizeof(s_title_cache[0]) - 1] = '\0';
            lv_label_set_text(s_lbl_sess_title[0], idle_txt);
        }
        // meta 缓存同样要清。
        //
        // 第 0 行的 meta（"project · 5m"）在空闲时是隐藏的，但缓存里还留着
        // 上一条会话的字符串。下一条会话如果恰好**同一个项目、同一段时长**，
        // 判等就会跳过 set_text，结果第 0 行下面的小字停在旧值上。
        // 时长一般会变（秒数在走），所以这个 bug 比标题那个更隐蔽 ——
        // 只在"切得极快、而且时长字符串没变"时露头。
        // 清成空串的成本是一次 strlen，可忽略。
        s_meta_cache[0][0] = '\0';

        lv_obj_set_style_text_color(s_lbl_sess_title[0], COL_DIM, 0);
        lv_obj_clear_flag(s_lbl_sess_title[0], LV_OBJ_FLAG_HIDDEN);
        // meta 行保持隐藏：空闲时不该出现"project · 5m"这种半截信息。
        // 上面循环已经 add_flag 隐藏过，这里不用重复。
    } else {
        lv_obj_set_style_text_color(s_lbl_sess_title[0], COL_TEXT, 0);
    }
}

// 第 1..N-1 屏主体：页标题 + 最多 6 行
static void ui_render_list(const board_data_t *d)
{
    char tb[32];
    if (d->page_title[0]) {
        utf8_sanitize(&font_cn_16, d->page_title, tb, sizeof(tb));
    } else {
        tb[0] = '\0';
    }
    if (strcmp(tb, s_page_title_cache) != 0) {
        strncpy(s_page_title_cache, tb, sizeof(s_page_title_cache) - 1);
        s_page_title_cache[sizeof(s_page_title_cache) - 1] = '\0';
        lv_label_set_text(s_lbl_page, tb);
    }

    for (int i = 0; i < MAX_ROWS; i++) {
        if (i >= d->row_count) {
            lv_obj_add_flag(s_lbl_row_t[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_row_v[i], LV_OBJ_FLAG_HIDDEN);
            s_row_t_cache[i][0] = '\0';
            s_row_v_cache[i][0] = '\0';
            continue;
        }

        char tt[TEXT_ROW], vv[TEXT_ROWVAL];
        utf8_sanitize(&font_cn_16, d->rows[i].t, tt, sizeof(tt));
        utf8_sanitize(&font_cn_12, d->rows[i].v, vv, sizeof(vv));

        // 三样都判等：文本变了才 set_text，颜色变了才改样式。
        // 渲染每帧都跑，不判等会一直触发重绘把 SPI 占满。
        if (strcmp(tt, s_row_t_cache[i]) != 0) {
            strncpy(s_row_t_cache[i], tt, sizeof(s_row_t_cache[i]) - 1);
            s_row_t_cache[i][sizeof(s_row_t_cache[i]) - 1] = '\0';
            lv_label_set_text(s_lbl_row_t[i], tt);
        }
        if (strcmp(vv, s_row_v_cache[i]) != 0) {
            strncpy(s_row_v_cache[i], vv, sizeof(s_row_v_cache[i]) - 1);
            s_row_v_cache[i][sizeof(s_row_v_cache[i]) - 1] = '\0';
            lv_label_set_text(s_lbl_row_v[i], vv);
        }
        if (s_row_c_cache[i] != d->rows[i].color) {
            s_row_c_cache[i] = d->rows[i].color;
            lv_color_t cc = row_color(s_row_c_cache[i]);
            lv_obj_set_style_text_color(s_lbl_row_t[i], cc, 0);
            lv_obj_set_style_text_color(s_lbl_row_v[i], cc, 0);
        }

        lv_obj_clear_flag(s_lbl_row_t[i], LV_OBJ_FLAG_HIDDEN);
        if (vv[0]) lv_obj_clear_flag(s_lbl_row_v[i], LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(s_lbl_row_v[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// 图表屏的判等缓存。
//
// 和列表屏同一个理由：渲染每帧都跑，每次都 set_value/set_text 会一直
// 触发重绘把 SPI 占满，而这三个数 5 秒才变一次。分开存 value / color /
// 名称，是为了让"只有颜色变了"这种更新也能命中，不用连带重设数字。
static int     s_chart_v_cache[MAX_CHARTS]       = { -1, -1, -1 };
static uint8_t s_chart_c_cache[MAX_CHARTS]       = { 0xFF, 0xFF, 0xFF };
static char    s_chart_n_cache[MAX_CHARTS][12];
static char    s_chart_title_cache[24];
static char    s_chart_f1_cache[24];
static char    s_chart_f2_cache[24];

// 图表屏主体：三个环形仪表 + 环下名称 + 底部进程/运行。
//
// 名称由服务端下发（"CPU"/"内存"/"磁盘"），固件不认识指标语义 ——
// 想换指标、换叫法、换顺序，只改服务端。
static void ui_render_chart(const board_data_t *d)
{
    char tb[24];
    utf8_sanitize(&font_cn_16, d->page_title, tb, sizeof(tb));
    if (strcmp(tb, s_chart_title_cache) != 0) {
        strncpy(s_chart_title_cache, tb, sizeof(s_chart_title_cache) - 1);
        s_chart_title_cache[sizeof(s_chart_title_cache) - 1] = '\0';
        lv_label_set_text(s_lbl_chart_title, tb);
    }

    for (int i = 0; i < MAX_CHARTS; i++) {
        if (i >= d->chart_count) {
            lv_obj_add_flag(s_arc_chart[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_chart_v[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_chart_k[i], LV_OBJ_FLAG_HIDDEN);
            s_chart_v_cache[i]   = -1;
            s_chart_c_cache[i]   = 0xFF;
            s_chart_n_cache[i][0] = '\0';
            continue;
        }
        lv_obj_clear_flag(s_arc_chart[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_chart_v[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_chart_k[i], LV_OBJ_FLAG_HIDDEN);

        if (s_chart_v_cache[i] != d->charts[i].v) {
            s_chart_v_cache[i] = d->charts[i].v;
            lv_arc_set_value(s_arc_chart[i], d->charts[i].v);
            lv_label_set_text_fmt(s_lbl_chart_v[i], "%d%%", d->charts[i].v);
        }
        if (s_chart_c_cache[i] != d->charts[i].color) {
            s_chart_c_cache[i] = d->charts[i].color;
            lv_color_t cc = chart_color(s_chart_c_cache[i]);
            lv_obj_set_style_arc_color(s_arc_chart[i], cc, LV_PART_INDICATOR);
            // 环内数字跟着变色：88% 是白的，91% 直接变红，扫一眼就知道
            lv_obj_set_style_text_color(s_lbl_chart_v[i], cc, 0);
        }

        char nb[12];
        utf8_sanitize(&font_cn_12, d->charts[i].n, nb, sizeof(nb));
        if (strcmp(nb, s_chart_n_cache[i]) != 0) {
            strncpy(s_chart_n_cache[i], nb, sizeof(s_chart_n_cache[i]) - 1);
            s_chart_n_cache[i][sizeof(s_chart_n_cache[i]) - 1] = '\0';
            lv_label_set_text(s_lbl_chart_k[i], nb);
        }
    }

    char f1[24], f2[24];
    utf8_sanitize(&font_cn_12, d->foot[0], f1, sizeof(f1));
    utf8_sanitize(&font_cn_12, d->foot[1], f2, sizeof(f2));
    if (strcmp(f1, s_chart_f1_cache) != 0) {
        strncpy(s_chart_f1_cache, f1, sizeof(s_chart_f1_cache) - 1);
        s_chart_f1_cache[sizeof(s_chart_f1_cache) - 1] = '\0';
        lv_label_set_text(s_lbl_chart_l, f1);
    }
    if (strcmp(f2, s_chart_f2_cache) != 0) {
        strncpy(s_chart_f2_cache, f2, sizeof(s_chart_f2_cache) - 1);
        s_chart_f2_cache[sizeof(s_chart_f2_cache) - 1] = '\0';
        lv_label_set_text(s_lbl_chart_r, f2);
    }
}

// 切页瞬间清空详情屏：否则会闪一下上一屏的内容。缓存也要一起清，
// 不然新数据和旧缓存"相等"，label 就永远停在空白状态。
static void ui_list_reset(void)
{
    for (int i = 0; i < MAX_ROWS; i++) {
        s_row_t_cache[i][0] = '\0';
        s_row_v_cache[i][0] = '\0';
        s_row_c_cache[i]    = 0xFF;      // 0xFF 不等于任何合法颜色，强制重设
        lv_label_set_text(s_lbl_row_t[i], "");
        lv_label_set_text(s_lbl_row_v[i], "");
        lv_obj_add_flag(s_lbl_row_t[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_row_v[i], LV_OBJ_FLAG_HIDDEN);
    }
    strcpy(s_page_title_cache, "加载中");
    lv_label_set_text(s_lbl_page, "加载中");
}

// 切换屏幕：只切容器可见性，不销毁控件
// 宠物精灵要不要显示。
//
// 宠物是浮在全屏底层的 120x96 元素，占 x=124..243 / y=148..243 ——
// 也就是屏幕的右下四分之一。图表屏的第三个环（磁盘，x=162..223）和
// 底部两行（"进程/运行"，y=185）全都落在这个范围里，宠物会压住它们：
// 实测"磁盘"标签和"运行 77m03s"被猫身糊掉，页脚时间也被压得看不清。
//
// 图表屏是纯数据面板，宠物（表情=工作状态）在这一屏没有语义价值，
// 藏掉它换文字可读性是划算的。概览屏/列表屏保持原样。
static bool pet_should_show(void)
{
    if (s_page <= 0 || s_page >= MAX_PAGES) return true;
    return !s_page_is_chart[s_page];
}

// ===========================================================================
// 空转休眠
// ===========================================================================

// 进入/退出休眠屏。
//
// 这里**只动可见性和背光**，不改 s_page、不销毁控件：
//   - s_page 保持原值，唤醒后立刻回到用户之前看的那一屏
//   - 控件全部复用，来回切换没有内存抖动
//
// 背光直接调 board_display_backlight_set()，**不碰 s_bright_idx**：
// 那是用户按键调的档位（40/70/100），休眠亮度是系统行为。
// 如果这里改 s_bright_idx，用户休眠前是 70%，醒来就变成 30% 了，
// 下次按键还会从这个被污染的值继续调 —— 属于看不见的状态污染。
static void sleep_apply(bool on)
{
    if (!s_pg_sleep) return;             // UI 还没建好（开机早期）
    if (on == s_asleep) return;          // 状态没变，不重复改控件

    s_asleep = on;

    if (on) {
        // ---- 藏：页头、页脚、两处页面容器、宠物、浮层都不动 ----
        //
        // 页头那几个控件（logo/时钟/日期/状态点/电量圈/充电标记/分隔线）
        // 是直接建在 scr 上的，没有统一容器，只能逐个藏。
        // 两个 mk_hline 建在 scr 上但没留句柄 —— 它们只是 1px 的线，
        // 被 s_pg_sleep 的不透明底整块盖住了，不用管。
        lv_obj_add_flag(s_lbl_clock,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_date,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_arc_bat,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_chrg,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_sys,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_footer, LV_OBJ_FLAG_HIDDEN);
        pet_view_set_visible(false);
        lv_obj_add_flag(s_pg_main,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_list,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_chart,   LV_OBJ_FLAG_HIDDEN);

        // logo 是个局部变量，没存句柄。它被 s_pg_sleep 的纯黑底盖住，
        // 不需要单独藏 —— 这也是给容器铺不透明底的主要原因之一。

        lv_obj_clear_flag(s_pg_sleep, LV_OBJ_FLAG_HIDDEN);
        // 眼睛定时器这时候才启动：醒着的时候一帧都不算，省的是实打实的 CPU
        eyes_enter();
        // ⚠ 背光**不在这里设**。省电分三级，亮度也是三档（30% / 5%），
        // 这里写死会跟 power_save_set(POWER_DEEP) 打架 —— 谁后调谁生效，
        // 而调用顺序取决于 idle_ms 落在哪个区间，行为会飘。
        // 由 sleep_update 在调完本函数后立刻 power_save_set() 决定。
        ESP_LOGI(TAG, ">>> 进入省电模式（界面已切到时钟屏）");
    } else {
        // ---- 唤醒：把可见性还给"正常界面" ----
        // 眼睛先停：它每 50ms 会 invalidate 一次画布，
        // 不在唤醒的第一时间掐掉，会跟主界面抢 SPI
        eyes_leave();
        lv_obj_add_flag(s_pg_sleep, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_clock,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_date,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_arc_bat,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_chrg,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_sys,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_footer, LV_OBJ_FLAG_HIDDEN);

        // 三个容器必须"先全藏再点亮一个"，和 page_apply 里的顺序一致。
        // 少这一步的话：休眠前在图表屏 → 休眠时按键切到列表屏 →
        // 唤醒时点亮 s_pg_list，但 s_pg_chart 仍留在可见状态
        // （休眠时它被 sleep_apply 藏了、可 page_apply 的休眠分支
        //   并没有再次藏它 —— 虽然实际已被藏，但依赖别处的副作用太脆）。
        // 显式全藏一遍是幂等的，代价可忽略，换来的是"点心原则"：
        // 谁点亮谁负责，不推测别人已经藏好。
        lv_obj_add_flag(s_pg_main,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_list,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);

        if (s_page == 0) {
            lv_obj_clear_flag(s_pg_main, LV_OBJ_FLAG_HIDDEN);
        } else if (s_page < MAX_PAGES && s_page_is_chart[s_page]) {
            lv_obj_clear_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
        }

        // 宠物：图表屏仍然要藏（pet_should_show 的规则）
        pet_view_set_visible(pet_should_show());

        // 背光恢复成用户调的档位
        board_display_backlight_set(k_bright_steps[s_bright_idx]);
        /* 电源级别跟着恢复。
         *
         * 顺序要紧：必须先 sleep_apply 把界面还原（上面那些可见性），
         * 再放功耗。反过来的话，power_save_set(ACTIVE) 出去的那一瞬间
         * 屏还是时钟屏 + 5% 背光，会闪一下黑的。
         *
         * 为什么放在这里而不是 ui_board_wake：唤醒有三个入口
         * （按键、来了任务、数据恢复在线），三个都得恢复功耗，
         * 放在共用的出口只写一次。 */
        power_save_set(POWER_ACTIVE);
        // 文案不写"有任务"：唤醒有两个来源（来了任务 / 用户按键），
        // 写死成前者会让人排查时误以为数据在变。
        ESP_LOGI(TAG, ">>> 退出省电模式（背光 %u%%）", k_bright_steps[s_bright_idx]);
    }
}

// 根据最新数据决定该不该进省电模式。每轮渲染都调用。
//
// ⚠ d->page == 0 这个守卫不能省，理由见 s_asleep 的注释。
// 注意同时要求 s_page == 0：数据属于第 0 屏、但用户已经按键切走了的话，
// 屏幕上是详情屏，这时候休眠会把人正在看的内容藏掉。
static void sleep_update(const board_data_t *d)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // ---- 有任务 / 离线：立刻退出，且清零计时 ----
    //
    // 两种情况都立刻退出，但语义不同：
    //   有任务     —— 屏幕本就是给人看进度的，藏起来毫无道理
    //   离线/降级  —— 这是需要被看到的异常。如果停在"● 在线"上而实际
    //                 早就断了，是最糟的误导
    bool ok     = d->ok;
#ifdef WB_TEST_IGNORE_BUSY
    /* 测试开关：忽略 working/session，用来实测省电三档。
     * 正常干活时设备永远不空闲（working 一直 >0），否则没法验证档位切换。
     * 正式固件不定义这个宏，行为完全不变。 */
    bool busy   = false;
#else
    bool busy   = ok && ((d->working > 0) || (d->session_count > 0));
#endif
    bool here   = ok && (d->page == 0) && (s_page == 0);

    if (!ok || busy) {
        // 清零计时：等活干完了要重新数满 3 分钟/10 分钟，而不是
        // "上次闲过一会儿、一停就睡"。这是"空闲满 N 分钟"的字面含义。
        s_idle_tracking = false;
        if (s_asleep) sleep_apply(false);
        return;
    }

    if (!here) {
        // 人在详情屏：既不该睡，也不该开始计时 —— 他正在看内容。
        s_idle_tracking = false;
        if (s_asleep) sleep_apply(false);
        return;
    }

    // ---- 走到这里：在线 + 空闲 + 停在第 0 屏 ----
    //
    // ⚠ 这里**不能**因为 s_asleep 就提前 return。原来写的是
    // `if (s_asleep) return;` —— 那样睡着之后就不再往下走了，
    // 永远进不了 DEEP 级（LIGHT → DEEP 的升级路径被这个 return 掐死）。
    // 睡着只是"界面已切到时钟屏"，空闲计时还得继续跑。

    if (!s_idle_tracking) {
        s_idle_tracking = true;
        s_idle_since_ms = now;
        ESP_LOGI(TAG, ">>> 空闲开始计时，%d 分钟后进省电模式",
                 (int)(SLEEP_IDLE_MS / 60000));
        return;
    }

    // 有符号差比较：uint32 毫秒在 49.7 天回绕，无符号相减在回绕点会
    // 变成一个巨大的正数，导致"刚进计时就满一分钟"的诡异行为。
    uint32_t idle_ms = (uint32_t)(int32_t)(now - s_idle_since_ms);

    /* ---- 三级递进 ----
     *
     * 从深到浅判断，命中就 return —— 空闲越久级别越深，所以先问最深的。
     * 用 if/return 而不是 if/else-if，是因为前面那个 `if (!s_asleep)`
     * 需要在两级分支里各写一份：第一次跨门槛才切界面，之后升级只改功耗。
     *
     * 一次跨两个门槛也可能发生（开机就没人动、计时一直没清零），
     * 那时应该直接进 DEEP，不该先假装进一下 LIGHT 再降级 ——
     * 两级都会调 sleep_apply(true)，但它是幂等的（s_asleep 已为 true
     * 就直接 return），所以不会多闪一次。
     *
     * sleep_apply() 只管"睡不睡"（界面切换），power_save_set() 管
     * "省到什么程度"。两者分开是因为界面状态机只认睡着/醒着两态，
     * 而功耗分三级 —— 硬塞进一个枚举会让 sleep_apply 里到处是 if。
     */
    if (idle_ms >= DEEP_IDLE_MS) {
        if (!s_asleep) sleep_apply(true);
        power_save_set(POWER_DEEP);
        return;
    }

    if (idle_ms >= SLEEP_IDLE_MS) {
        if (!s_asleep) sleep_apply(true);
        power_save_set(POWER_LIGHT);
        return;
    }
}

static void page_apply(int page)
{
    int nscr = board_data_nscr();
    if (nscr < 1) nscr = 1;
    page = ((page % nscr) + nscr) % nscr;   // 环形，负数也安全
    if (page == s_page) return;

    s_page = page;

    // 休眠中切屏：只更新 s_page，**不动可见性**。
    //
    // 按键唤醒的顺序是 page_apply → sleep_apply(false)（见 lvgl_task），
    // 所以这里如果把页面容器点亮，休眠屏的纯黑底就会被戳穿：
    // s_pg_sleep 在子对象顺序里更靠后（更靠上），视觉上会盖住，
    // 但它下面那层的宠物和页头控件是隐藏状态 —— 唤醒时 sleep_apply
    // 会按 s_page 正确恢复，所以这里什么都不做才是对的。
    if (s_asleep) {
        // 但缓存必须照常清。不清的话，唤醒时点亮的列表屏里装的是
        // **上一屏**的行文字，要等下一轮数据（最多 5 秒）才刷掉，
        // 用户按下键先看到的是一屏张冠李戴的内容。
        // 这里和正常路径的唯一差别就是"不点亮容器"。
        if (page != 0 && !(page < MAX_PAGES && s_page_is_chart[page])) {
            ui_list_reset();
        }
        data_fetch_kick();
        ESP_LOGI(TAG, ">>> 省电模式中切屏 -> %d/%d（暂不显示）", page, board_data_nscr());
        return;
    }

    // 三个容器先全部隐藏再点亮一个。只显示不隐藏的话，从图表屏切回列表屏
    // 会两层叠着画 —— 图表是透明底的，底下的字会透上来。
    lv_obj_add_flag(s_pg_main,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pg_list,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);

    if (page == 0) {
        lv_obj_clear_flag(s_pg_main, LV_OBJ_FLAG_HIDDEN);
    } else if (page < MAX_PAGES && s_page_is_chart[page]) {
        lv_obj_clear_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);
    } else {
        // 类型还不知道（首次切到该屏）或本来就是文本屏，都先按列表渲染。
        // 数据一到，渲染发现 kind=chart 会立刻切到图表容器。
        ui_list_reset();
        lv_obj_clear_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
    }

    // 切屏后立刻同步宠物的可见性。图表屏藏宠物，避免它压住环下标签。
    // 重新显示时模块内部会补一帧，让表情立刻对上当前状态。
    pet_view_set_visible(pet_should_show());

    // 叫醒拉取任务立刻换 URL 重拉，不等下一个 5 秒周期
    data_fetch_kick();
    ESP_LOGI(TAG, ">>> 切屏 -> %d/%d", page, board_data_nscr());
}

// 电量圆圈渲染。只在 LVGL 任务里调用（和渲染同线程）。
//
// 只画一圈弧，**不显示百分比数字**。弧长即电量。
//
// 分档着色：
//   >50%  绿   20~50%  黄   <20%  红
// 充电中：进度弧固定绿色，圆心显示 "+" 标记。
//
// 注意 percent 与 volt_pct 的区别（见 battery.h）：
//   percent  = 显示用，充电时是"充电前的真实电量"，不会虚高到 100%
//   volt_pct = 纯电压换算，充电时虚高，只用于调试日志
static void ui_render_battery(void)
{
    battery_state_t b;
    battery_get(&b);

    // 判等缓存：每帧 set_value / set_text 都会触发重绘，
    // 5 秒一轮刷新时百分比通常不变，跳过能省下 SPI 带宽。
    // 现在没有数字了，但弧长仍按 1% 变化，判等照样有意义。
    static int  last_pct  = -2;
    static bool last_chrg = false;

    if (!b.valid) {
        if (last_pct != -1) {
            last_pct = -1;
            lv_arc_set_value(s_arc_bat, 0);
            lv_obj_set_style_arc_color(s_arc_bat, COL_DIM, LV_PART_INDICATOR);
            lv_label_set_text(s_lbl_chrg, "");
        }
        return;
    }

    int pct = b.percent;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    if (pct != last_pct || b.charging != last_chrg) {
        last_pct  = pct;
        last_chrg = b.charging;

        lv_arc_set_value(s_arc_bat, pct);

        // 三档配色。充电中固定绿色，避免"充电时红光"的矛盾观感。
        // 去掉数字后，颜色成了**唯一**的告警通道 —— 低电量只能靠弧变红
        // 来提示，所以这里的三档必须保留，不能像原来那样"反正有数字"。
        lv_color_t col;
        if (b.charging)     col = COL_OK;
        else if (pct > 50)  col = COL_OK;
        else if (pct >= 20) col = COL_WARN;
        else                col = COL_ERR;

        lv_obj_set_style_arc_color(s_arc_bat, col, LV_PART_INDICATOR);

        // 充电标记：圆心里一个绿色 ⚡，放电时清空。
        //
        // 早期这里用的是 "+"，注释里写着"字体没有 ⚡"。那句注释是错的 ——
        // ⚡ (U+26A1) 一直在 EXTRA_SYMBOLS 里，由 seguisym.ttf 提供，
        // font_cn_12/16/28 三档都有字形。之前的判断八成是把它和
        // ° / ℃（那两个确实没有，见页脚温度的注释）混为一谈了。
        //
        // 尺寸实测（font_cn_12）：⚡ adv_w=120 → **8px** 宽、位图 7x10，
        // 而 "+" 是 7px 宽、位图 7x6。只宽 1px，圆环内径 18px 完全放得下，
        // 不会压到弧线。所以换符号不需要动任何几何常量。
        if (b.charging) {
            lv_obj_set_style_text_color(s_lbl_chrg, COL_OK, 0);
            lv_label_set_text(s_lbl_chrg, "⚡");
        } else {
            lv_label_set_text(s_lbl_chrg, "");
        }
    }
}

void ui_board_render(const board_data_t *d)
{
    // ---- 休眠判定（最先做：它可能把整屏换成一块时钟）----
    //
    // 放在电量/页头/页脚之前，是为了让"睡着"这一轮**直接返回**：
    // 那些控件都藏着，更新它们的文字是纯浪费 ——
    // 尤其每次 set_text 都会标脏重绘，240x240 SPI 刷一屏十几毫秒，
    // 而休眠屏只有在分钟跳变时才真的需要重绘。
    sleep_update(d);

    if (s_asleep) {
        // 大时钟：只在文字变化时才 set_text。
        // 不加这个判等的话，5 秒一轮的渲染会每分钟白白重绘 12 次，
        // 休眠本来就是为了省电，结果醒着刷屏就说不过去了。
        static char clock_cache[8] = "";
        if (strcmp(clock_cache, d->clock) != 0) {
            strncpy(clock_cache, d->clock, sizeof(clock_cache) - 1);
            clock_cache[sizeof(clock_cache) - 1] = '\0';
            lv_label_set_text(s_lbl_bigclock, d->clock);
        }
        // 状态点不用每轮改：能进休眠就说明 d->ok 为真，
        // 建屏时写好的"● 在线"一直是正确的。真掉线了 sleep_update
        // 会先把人叫醒，那时页头自己的"● 离线"显示出来。
        return;
    }

    // ---- 电量圆圈（独立于网络状态，离线时也照常更新）----
    ui_render_battery();

    // ---- 页头（两屏共用，随时更新）----
    if (!d->ok) {
        lv_obj_set_style_text_color(s_lbl_status, COL_ERR, 0);
        lv_label_set_text(s_lbl_status, "● 离线");
        char fbuf[96], ebuf[64];
        utf8_sanitize(&font_cn_12,
                      d->error[0] ? d->error : "无法连接 bridge",
                      ebuf, sizeof(ebuf));
        snprintf(fbuf, sizeof(fbuf), "✗ %s", ebuf);
        // 离线时页脚左侧让给错误原因（右侧更新时间清空，不然两条挤在一起）
        lv_obj_set_style_text_color(s_lbl_sys, COL_ERR, 0);
        lv_label_set_text(s_lbl_sys, fbuf);
        // 清缓存一起清：恢复在线后若新鲜度文本恰好和断网前一样，
        // 判等会拦住重绘，页脚就一直是空白
        s_footer_cache[0] = '\0';
        lv_label_set_text(s_lbl_footer, "");
        // 离线时宠物回到待机表情（慢眨眼）—— 它还在呼吸，只是没活干，
        // 比冻在最后一帧上更像"设备活着但连不上"。
        pet_view_set_state(PET_ST_IDLE);
        return;   // 保留上次数据，避免清零闪屏
    }

    lv_obj_set_style_text_color(s_lbl_status, COL_OK, 0);
    lv_label_set_text(s_lbl_status, "● 在线");

    lv_label_set_text(s_lbl_clock, d->clock);
    lv_label_set_text(s_lbl_date, d->date);

    // ---- 页脚（所有屏共用）----
    // 左：本机 CPU / 内存 / 磁盘占用。超阈值时整条变黄，
    // 12px 字号做不到逐段着色，整条变色足够提醒。
    //
    // 图表屏例外：屏上已经有三个环，页脚再来一串
    // "CPU 42%  MEM 68%  DSK 88%" 是同一份数据说两遍，还压在底部字上。
    bool chart_page = (s_page > 0 && s_page < MAX_PAGES && s_page_is_chart[s_page]);
    if (chart_page) {
        // 图表屏页脚左侧空着（三个环已经把 CPU/MEM/DSK 说过了），
        // 正好放芯片结温 —— 这几个环讲的是 PC 的状态，温度是设备自己的，
        // 放在同一行反而帮读者区分"屏上哪些数是电脑的、哪些是这块板子的"。
        // 温度取本地读数为准，服务端透传的那份只当兜底。
        // 原因：本地传感器 30 秒才上报一次、服务端再等下一轮 5 秒轮询才回传，
        // 走一趟链路最坏要 35 秒。而 device_report_chip_temp() 是上报里
        // 刚读到的值，屏幕 50ms 一刷就能跟上，反应快得多。
        // d->chip_temp 的意义是"服务端也拿到了这份数据"，供别的端看。
        int local_t = device_report_chip_temp();
        int t = (local_t >= 0) ? local_t : d->chip_temp;
        if (t < 0) {
            lv_label_set_text(s_lbl_sys, "板 --");
            lv_obj_set_style_text_color(s_lbl_sys, COL_DIM, 0);
        } else {
            char tbuf[24];
            // 标签用 "板" 而不是 "SoC"：
            //   这一屏上三个环讲的是 PC 的 CPU/内存/磁盘，页脚冒出 "SoC" 时
            //   读者会自然理解成"服务器那颗 SoC" —— 实际是这块板子自己的
            //   芯片结温。一个字的差别，但语义完全不同。
            //
            // 不用 "°C"：字库里没有 ° (U+00B0) 和 ℃ (U+2103)，
            // 渲染出来是方框。用 "度" —— 汉字有字形，且比裸 "C"
            // 更不容易被误读成型号（"板 48C" 看着像个编号）。
            snprintf(tbuf, sizeof(tbuf), "板 %d度", t);
            lv_label_set_text(s_lbl_sys, tbuf);
            // 结温超过 70°C 变黄：S3 的过热保护在 105°C 左右，
            // 70 是个"已经开始发烫但还正常"的提示线，不是告警
            lv_obj_set_style_text_color(s_lbl_sys,
                                        t >= 70 ? COL_WARN : COL_DIM, 0);
        }
    } else {
        char sbuf[56];
        if (d->cpu < 0) {
            // 旧版服务端没下发这些字段，明确显示 -- 而不是假的 0%
            snprintf(sbuf, sizeof(sbuf), "CPU --  MEM --");
        } else {
            int n = snprintf(sbuf, sizeof(sbuf), "CPU %d%%  MEM %d%%",
                             d->cpu, d->mem);
            if (d->disk >= 0 && n > 0 && n < (int)sizeof(sbuf) - 12) {
                snprintf(sbuf + n, sizeof(sbuf) - n, "  DSK %d%%", d->disk);
            }
        }
        lv_label_set_text(s_lbl_sys, sbuf);
        bool hot = (d->cpu >= 85) || (d->mem >= 90);
        lv_obj_set_style_text_color(s_lbl_sys, hot ? COL_WARN : COL_DIM, 0);
    }

    footer_render_age(d);

    // 宠物表情跟着数据走（本屏两页共用，放在这里只算一次）
    pet_view_update(d);

    // ---- 主体 ----
    // 回包可能属于"切页前"发出的那个请求（d->page 落后于 s_page），
    // 这时只更新页头页脚，主体等下一轮，别把上一屏的字画到新屏上
    if (d->page != s_page) return;

    // 服务端说这一屏是图表 → 记住类型并立刻换容器。
    // 首次切到某屏时固件还不知道类型（page_apply 只能按文本渲染），
    // 数据一到这里就切成图表；之后切回来直接走图表，不会闪。
    if (s_page > 0 && s_page < MAX_PAGES && d->is_chart != s_page_is_chart[s_page]) {
        s_page_is_chart[s_page] = d->is_chart;
        if (d->is_chart) {
            lv_obj_add_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, ">>> 屏 %d 类型 -> %s", s_page, d->is_chart ? "图表" : "文本");

        // 类型变了，宠物的可见性也得跟着变。
        // 首次切到系统屏时固件还不知道它是图表屏（page_apply 先按文本渲染），
        // 宠物这时是显示的；数据到、切成图表后必须补一次隐藏，
        // 否则猫会一直压在"磁盘"标签和"运行 xx"上面。
        pet_view_set_visible(pet_should_show());
    }

    if (s_page == 0)                                        ui_render_overview(d);
    else if (s_page < MAX_PAGES && s_page_is_chart[s_page]) ui_render_chart(d);
    else                                                    ui_render_list(d);
}

// ===========================================================================
// 按键动作
// ===========================================================================

// step: 0 = 循环（亮→暗→亮），正数 = 调亮，负数 = 调暗
//
// ⚠ 这个函数在**按键任务**里被调用，所以它只写状态和请求标志，
//   不碰任何 LVGL 对象 —— 真正改背光的是 ui_board_poll()。
void ui_board_brightness(const char *who, int step)
{
    int32_t idx = (int32_t)s_bright_idx;

    if (step == 0) {
        idx = (idx == 0) ? (int32_t)BRIGHT_STEP_MAX : idx - 1;
    } else {
        idx += step;
        if (idx < 0)                      idx = 0;
        if (idx > (int32_t)BRIGHT_STEP_MAX) idx = (int32_t)BRIGHT_STEP_MAX;
    }

    s_last_btn = who;
    if (idx == (int32_t)s_bright_idx) {       // 已经到头了，不重复提示
        ESP_LOGI(TAG, ">>> 按键命中 [%s] 亮度已到头，保持 %u%%",
                 who, k_bright_steps[s_bright_idx]);
        return;
    }

    s_bright_idx   = (uint32_t)idx;
    uint8_t pct    = k_bright_steps[idx];
    s_bright_apply = 1;
    s_toast_val    = pct;
    s_toast_until  = (uint32_t)(esp_timer_get_time() / 1000) + 1500;
    ESP_LOGI(TAG, ">>> 按键命中 [%s] 亮度 -> %u%%", who, pct);
}

// ===========================================================================
// WiFi 状态浮层同步
// ===========================================================================

// 只做"把 wifi_mgr 的状态翻译成屏幕文字"，不含任何连接判断逻辑。
// 状态口径全在 wifi_mgr.cpp 里，这边改文案不会影响连接行为。
//
// 只在状态变化时改控件：LVGL 每次 set_text 都会标脏重绘，
// 240x240 的 SPI 屏刷一次要十几毫秒，每帧无脑刷会把帧率拖下去。
static void wifi_ui_sync(void)
{
    static wifi_state_t last_st      = (wifi_state_t)-1;
    static int          last_clients = -1;

    wifi_state_t st = wifi_mgr_state();
    /* 配网模式下文案还跟"有没有手机连上来"有关，所以那一位也要参与比对，
     * 否则手机连上了屏幕不会变。 */
    int clients = (st == WIFI_ST_PROVISION) ? wifi_mgr_ap_clients() : 0;
    if (st == last_st && clients == last_clients) return;
    last_st = st;
    last_clients = clients;

    if (st == WIFI_ST_CONNECTED) {
        lv_obj_add_flag(s_pg_wifi, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // 连接中不遮挡：开机到连上只有两三秒，中途闪一块"未连接"很难看。
    // 真连不上会转 FAILED，那时再显示。
    if (st == WIFI_ST_CONNECTING) {
        lv_obj_add_flag(s_pg_wifi, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_pg_wifi, LV_OBJ_FLAG_HIDDEN);

    if (st == WIFI_ST_FAILED) {
        lv_obj_set_style_text_color(s_lbl_wifi_title, COL_ERR, 0);
        lv_label_set_text(s_lbl_wifi_title, "WiFi 未连接");

        lv_label_set_text(s_lbl_wifi_l1, wifi_mgr_ssid()[0] ? wifi_mgr_ssid() : "未配置网络");
        lv_obj_set_style_text_color(s_lbl_wifi_l1, COL_TEXT, 0);

        lv_label_set_text(s_lbl_wifi_l2, "长按 BOOT 3 秒");
        lv_obj_set_style_text_color(s_lbl_wifi_l2, COL_ACCENT, 0);

        lv_label_set_text(s_lbl_wifi_l3, "进入配网模式\n用手机配置 WiFi");
    } else {                                  // WIFI_ST_PROVISION
        /* 热点是开放网络（无密码），所以第二行不再显示密码，
         * 改成显示配置页地址 —— 这才是用户接下来真正要用的东西。 */
        lv_obj_set_style_text_color(s_lbl_wifi_title, COL_ACCENT, 0);
        lv_label_set_text(s_lbl_wifi_title, "配网模式");

        lv_label_set_text(s_lbl_wifi_l1, wifi_mgr_ap_ssid());
        lv_obj_set_style_text_color(s_lbl_wifi_l1, COL_TEXT, 0);

        /* 第二行固定显示配置页地址（这才是用户接下来真正要用的东西）。
         * 颜色和第三行文案随"手机有没有连上来"切换：
         *   没连   → 灰色地址 + 先教怎么连热点
         *   已连   → 绿色地址 + 直接让他打开浏览器
         * 不做成"密码"那一行了，因为热点已经改成开放网络。 */
        lv_label_set_text(s_lbl_wifi_l2, wifi_mgr_ap_ip());
        lv_obj_set_style_text_color(s_lbl_wifi_l2, clients > 0 ? COL_OK : COL_ACCENT, 0);

        lv_label_set_text(s_lbl_wifi_l3, clients > 0
                          ? "手机已连接\n点开浏览器访问上面的地址"
                          : "手机连上面这个热点\n（不需要密码）");
    }
}

// 服务地址浮层同步。同样是"只翻译，不判断"：连不上这件事由 wifi_mgr 的
// 失败计数说了算（连续 5 次），这里只负责把状态和地址显示出来。
//
// 两种状态共用一个浮层：
//   正在连接服务  ← WiFi 通了、服务还没拉通（首次连接，或连上过又断了）
//   服务连不上    ← 连续 5 次失败，此时附设备 IP + 验证码，让用户能改地址
//
// 参与比对的还有"地址版本号" —— 用户改了地址之后要立刻把新地址显示出来，
// 不然屏上还挂着旧 IP，会让人以为没改成功。
static void svr_ui_sync(void)
{
    static bool     last_down = false;
    static bool     last_wait = false;
    static uint32_t last_ver  = 0xFFFFFFFF;
    static char     last_ip[16] = "";
    static char     last_tok[8] = "";
    static int      last_left = -1;
    /* WiFi 连上的时刻。首连时靠它算"等 2 秒再提示"——
     * 正常开机这会儿首轮 HTTP 已经回来了，先等一等，屏幕就不会闪一下浮层。 */
    static bool     wifi_seen  = false;
    static uint32_t wifi_since = 0;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    bool wifi_ok = (wifi_mgr_state() == WIFI_ST_CONNECTED);
    if (!wifi_ok) {
        wifi_seen = false;
    } else if (!wifi_seen) {
        wifi_seen  = true;
        wifi_since = now;
    }

    // 只有 WiFi 通着时"连不上服务"才有意义：WiFi 都断了的话那是 WiFi 浮层的事
    bool down = wifi_mgr_svr_down() && wifi_ok;

    /* 正在连接服务。两种情况进来，时间阈值不同：
     *   · 一次都没连上过 —— WiFi 连上 2 秒后（正常开机那时早已拉通，不闪）
     *   · 连上过又断了     —— 连续失败 2 次（约 10 秒）才提示，单次超时不闪屏 */
    bool wait = wifi_ok && !down &&
                (wifi_mgr_svr_ever_ok()
                     ? (wifi_mgr_svr_fails() >= 2)
                     : ((uint32_t)(now - wifi_since) >= 2000));

    uint32_t   ver  = wifi_mgr_svr_version();
    const char *ip  = wifi_mgr_ip();
    /* 验证码 5 分钟有效，所以它也要参与比对 —— 不然新码上了屏，
     * 用户还照着旧码填，提交被拒却不知道为什么。
     * 剩余秒数（倒计时）同样参与：它每秒都变，变了就要重画。 */
    const char *tok  = down ? wifi_mgr_cfg_token() : "";
    int         left = down ? wifi_mgr_cfg_token_left() : 0;

    // IP 也参与比对：路由器重新分了 IP 的话，屏上那个配置页地址得跟着变
    if (down == last_down && wait == last_wait && ver == last_ver
        && strcmp(ip, last_ip) == 0 && strcmp(tok, last_tok) == 0
        && left == last_left) return;
    last_down = down;
    last_wait = wait;
    last_ver  = ver;
    last_left = left;
    strncpy(last_ip, ip, sizeof(last_ip) - 1);
    strncpy(last_tok, tok, sizeof(last_tok) - 1);

    /* 只在状态切换时打一条：屏幕此刻显示什么，串口日志里能对上。
     * 现场排查"一直卡在正在连接"时，比盯着屏幕猜有用。 */
    ESP_LOGI(TAG, "服务浮层: %s", wait ? "正在连接" : (down ? "连不上" : "隐藏"));

    if (!down && !wait) {
        lv_obj_add_flag(s_pg_svr, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_pg_svr, LV_OBJ_FLAG_HIDDEN);

    // 省掉 http:// 前缀：240px 屏上一行要塞下 IP 和端口，前缀没信息量
    const char *b = wifi_mgr_svr_base();
    if (strncmp(b, "http://", 7) == 0) b += 7;

    if (wait) {
        /* 连接中：蓝色标题，明确写"正在连接服务"，下一行就是要连的地址。
         * 不显示验证码和配置页地址 —— 这会儿配置页还没开，也没有码可填。 */
        lv_obj_set_style_text_color(s_lbl_svr_title, COL_ACCENT, 0);
        lv_label_set_text(s_lbl_svr_title, "正在连接服务");
        lv_label_set_text(s_lbl_svr_l1, b[0] ? b : "未配置");
        lv_label_set_text(s_lbl_svr_l2, "");
        lv_label_set_text(s_lbl_svr_l3, "请确认电脑上的服务已启动\n连上后会自动进入看板");
        lv_label_set_text(s_lbl_svr_tok, "");
        return;
    }

    /* 连不上：标题恢复红色。两种状态共用一套控件，切回来必须显式改，
     * 不能指望 build 时的初值还在。 */
    lv_obj_set_style_text_color(s_lbl_svr_title, COL_ERR, 0);
    lv_label_set_text(s_lbl_svr_title, "服务连不上");

    lv_label_set_text(s_lbl_svr_l1, b[0] ? b : "未配置");

    // 第二行是用户真正要用的东西：设备自己在局域网里的 IP = 配置页地址
    lv_label_set_text(s_lbl_svr_l2, ip[0] ? ip : "--");

    // 第三行提示 + 验证码。配置页无鉴权（跑在真实局域网上），
    // 改服务地址必须带这个码 —— 屏幕是唯一的分发渠道。
    // 后面跟剩余有效期（分:秒）：用户能看到还有多久，抄码时心里有数，
    // 不会填到一半失效却不知道为什么被拒。
    if (tok[0]) {
        lv_label_set_text(s_lbl_svr_l3, "用浏览器打开上面地址\n照着下面的数字填验证码");
        char buf[40];
        snprintf(buf, sizeof(buf), "验证码 %s · %d:%02d", tok,
                 left / 60, left % 60);
        lv_label_set_text(s_lbl_svr_tok, buf);
    } else {
        lv_label_set_text(s_lbl_svr_l3, "用浏览器打开上面地址\n进去改服务地址");
        lv_label_set_text(s_lbl_svr_tok, "");
    }
}

// ===========================================================================
// 每帧轮询：消费请求 + 同步浮层
// ===========================================================================

void ui_board_poll_input(void)
{
    // 按键产生的亮度变更，统一在 LVGL 任务里落实
    if (s_bright_apply) {
        s_bright_apply = 0;
        /* ⚠ 省电模式中**只记档位、不马上应用**。
         *
         * 原来这里无条件 set 背光。省电模式下按键调亮度会让屏幕
         * 突然从 5% 跳到 70%，然后又因为还闲着被 idle 逻辑压回 5%
         * —— 闪一下很怪，而且用户看不到"我调的档位生效了"。
         *
         * 现在：档位照记（s_bright_idx 已由按键任务改好），
         * 点亮要等唤醒。醒来时 sleep_apply(false) 会用这个新档位。
         */
        if (!s_asleep) {
            board_display_backlight_set(k_bright_steps[s_bright_idx]);
        }
    }

    // 切屏请求也在这里执行：LVGL 的控件操作必须留在本任务，
    // 按钮任务只负责投递 s_page_req
    {
        int req = s_page_req;
        if (req >= 0) {
            s_page_req = -1;
            page_apply(req);
        }
    }

    // 按键唤醒：省电模式中按键，先把人叫起来。
    //
    // 顺序有讲究 —— 必须在 page_apply 之后执行：
    // 用户可能是"想看下一屏"而按的键，page_apply 已经把人送到新屏了，
    // 这里只需要把屏幕点亮。反过来先唤醒再切屏会出现一帧的跳动。
    //
    // 清掉空闲计时：退出后要重新数满一分钟才睡回去。
    // 不清的话 s_idle_since_ms 还停在睡着之前的时刻，
    // 下一轮 sleep_update 一算差值早就超过 60 秒，立刻又睡 —— 按键等于白按。
    if (s_wake_req) {
        s_wake_req      = false;
        s_idle_tracking = false;
        sleep_apply(false);
    }
}

void ui_board_poll_status(void)
{
    // 按键提示：显示 1.5 秒后自动消失
    if (s_toast_val >= 0) {
        static uint32_t last_toast = 0xFFFFFFFF;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now < s_toast_until) {
            if (last_toast != (uint32_t)s_toast_val) {
                last_toast = (uint32_t)s_toast_val;
                lv_label_set_text_fmt(s_lbl_toast, "☀ %d%%", s_toast_val);
                lv_obj_clear_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);
            last_toast   = 0xFFFFFFFF;
            s_toast_val  = -1;
        }
    }

    // WiFi 状态变了就更新浮层（内部有状态去重，每帧调用开销可忽略）
    wifi_ui_sync();
    svr_ui_sync();
}

// ===========================================================================
// 对外查询 / 动作
// ===========================================================================

int ui_board_page(void)
{
    return s_page;
}

bool ui_board_asleep(void)
{
    return s_asleep;
}

const char *ui_board_last_btn(void)
{
    return (const char *)s_last_btn;
}

int ui_board_brightness_pct(void)
{
    return k_bright_steps[s_bright_idx];
}

void ui_board_next_page(void)
{
    s_page_req = s_page + 1;
}

void ui_board_wake(void)
{
    s_wake_req = true;
}
