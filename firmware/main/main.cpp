// WorkBuddy 状态看板 · 星智 1.54TFT WiFi (ESP32-S3)
//
// 设计说明
// --------
// 本固件只复用星智 1.54TFT 的硬件引脚定义（见 board_config.h），
// 不继承小智 xiaozhi-esp32 的语音链路。原因：
//   小智的 Application 状态机与 LcdDisplay 界面树（top_bar_/content_/emoji_box_）
//   全部围绕语音对话构建。看板是纯数据展示，套进去需要拆掉大半，
//   且对话态会覆盖看板内容。硬件参考 > 代码继承。
//
// 数据流：WiFi -> HTTP GET bridge(/status) -> 解析 JSON -> LVGL 渲染 ST7789
//
// 引脚来源：78/xiaozhi-esp32 main/boards/nologo/xingzhi-cube-1.54tft-wifi/config.h
// 芯片实测：ESP32-S3 QFN56 rev0.2 / 8MB Octal PSRAM / 16MB Flash (esptool flash_id)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/temperature_sensor.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"

#include "wifi_mgr.h"

#include "lvgl.h"
#include "board_config.h"
#include "battery.h"
#include "pet.h"
#include "audio.h"

// 中文字体（由 tools/gen_charset.py + lv_font_conv 生成）
LV_FONT_DECLARE(font_cn_12);
LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_cn_28);
// 72px 只含 "0123456789:"。空转休眠屏的大时钟专用 ——
// 只为了这一处显示时间就背一整套 72px 中文字库要多花 500KB Flash，
// 而它这辈子只会显示 HH:MM 五个字符。
LV_FONT_DECLARE(font_cn_72);

static const char *TAG = "wb-board";

// ===========================================================================
// 字体自检：LVGL 找不到字形时会画占位方格，这里直接问 LVGL 到底认不认这些字
// ===========================================================================
static void font_probe(void)
{
    static const struct { uint32_t cp; const char *name; } list[] = {
        { 0x20,   "空格" },
        { 0x5DE5, "工(中文)" },
        { 0x2605, "★" },
        { 0x26A1, "⚡" },
        { 0x2713, "✓" },
        { 0x2600, "☀" },
        { 0x25CF, "●" },
    };

    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        lv_font_glyph_dsc_t d;
        memset(&d, 0, sizeof(d));
        bool ok = lv_font_get_glyph_dsc(&font_cn_16, &d, list[i].cp, 0);

        /* ⚠ 只有"真的缺字"才告警。
         * 原来每次开机都打 7 条 WARN，正常字形也照打 —— 真正的告警
         * 混在里面，等于没有告警。现在：正常的降级成 DEBUG（默认不输出，
         * 需要排查时把日志级别调到 DEBUG 就能看到完整自检结果），
         * 缺字形（found=0 或渲染出来是占位方格）才升成 WARN。 */
        bool bad = (!ok) || d.is_placeholder;
        if (bad) {
            ESP_LOGW(TAG, "FONT %-9s U+%04X 缺失！found=%d placeholder=%d",
                     list[i].name, (unsigned)list[i].cp, (int)ok, (int)d.is_placeholder);
        } else {
            ESP_LOGD(TAG, "FONT %-9s U+%04X ok box=%dx%d ofs=(%d,%d) adv=%u",
                     list[i].name, (unsigned)list[i].cp,
                     (int)d.box_w, (int)d.box_h, (int)d.ofs_x, (int)d.ofs_y,
                     (unsigned)d.adv_w);
        }
    }
}

// 服务端下发的是任意字符串（文件名/项目名可能是中文），而字库只烘焙了
// 固定字符集。LVGL 遇到字库里没有的字会画一个占位方框，很难看。
// 这里先按 UTF-8 逐字检查，字库没有的降级成 '?'，保证界面上永不出现方框。
static void utf8_sanitize(const lv_font_t *font, const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;

    while (*p && o + 1 < out_len) {
        uint32_t cp;
        int len;

        if (*p < 0x80)             { cp = *p;        len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { p++; continue; }

        bool valid = true;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }

        lv_font_glyph_dsc_t d;
        memset(&d, 0, sizeof(d));
        bool have = lv_font_get_glyph_dsc(font, &d, cp, 0)
                    && !d.is_placeholder && d.adv_w > 0;

        if (have && o + (size_t)len < out_len) {
            memcpy(out + o, p, len);
            o += len;
        } else if (o + 1 < out_len) {
            out[o++] = '?';
        }
        p += len;
    }
    out[o] = '\0';
}

// ===========================================================================
// 用户配置
// ===========================================================================
// config.local.h 由 configure.py 生成（不进版本库）。
// 它覆盖下面的默认值，因此改 WiFi 不用动本文件。
#if __has_include("config.local.h")
#include "config.local.h"
#endif

// ⚠ 这里**没有** WIFI_SSID / WIFI_PASSWORD。
// 2026-10-27 起固件只从 NVS 读 WiFi 凭据，没有就开机进配网模式。
// 以前留着的这两个宏已经是死代码（wifi_mgr 不读、configure.py 也不写），
// 留着只会让下一个改代码的人以为"改这里就能换 WiFi"—— 改了完全没效果。
//
// 服务地址（本机以太网 IP，设备必须连同一个路由器）
// 只给 base：第 0 屏走 /status，其余屏走 /screen/<i>，路径由 s_page 决定
// 端口 27777 与服务端 DEFAULT_PORT 保持一致（刻意避开 8080）
//
// ⚠ 这只是**出厂兜底值**。2026-10-27 起地址存在 NVS 里，配过一次就以 NVS
// 为准：电脑换 IP 时在配置页改一下就行，不用改这里、也不用重烧。
// 配置页地址见屏幕上的"服务连不上"提示（= 设备自己的 IP）。
#ifndef BRIDGE_BASE
#define BRIDGE_BASE       "http://192.168.0.89:27777"
#endif
#define BRIDGE_PATH_MAIN  "/status"

#define POLL_INTERVAL_MS  (5 * 1000)

// ===========================================================================
// 数据模型
// ===========================================================================

#define MAX_SESSIONS 3
#define TEXT_TITLE   40
#define TEXT_PROJECT 20
#define TEXT_ELAPSED 12

// 详情屏：服务端下发"标签 + 右值"的行，固件只负责画，不认识业务语义。
// 想改每屏显示什么只改服务端 _screen_rows()，固件不用动。
#define MAX_ROWS     6
#define TEXT_ROW     40        // 行主文本，16px 字体约 13 个中文
#define TEXT_ROWVAL  16        // 右侧数值，12px 字体

// 图表屏：服务端下发 kind="chart" + charts 数组，固件画成环形仪表。
// 3 个是屏幕宽度决定的 —— 62px 环 × 3 + 间距 11 × 2 = 208，刚好落在 224 的
// 内容区里。想加第四个环只能缩到 46px，内径装不下 "100%"。
#define MAX_CHARTS   3
// 屏号上限，只为 s_page_is_chart[] 定长。服务端 nscr 目前 5，
// 留一倍余量，真加到 8 屏也不用改这里。
#define MAX_PAGES    8

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

typedef struct {
    char    t[TEXT_ROW];
    char    v[TEXT_ROWVAL];
    uint8_t color;             // 0 普通 / 1 强调蓝 / 2 成功绿 / 3 警告黄 / 4 暗灰
} row_t;

typedef struct {
    char    k[8];              // 指标键 cpu/mem/disk，仅用于日志和调试
    char    n[12];             // 环下名称（服务端给中文，固件不认语义）
    int     v;                 // 0..100
    uint8_t color;             // 1 蓝 / 2 绿 / 3 黄 / 5 红
} chart_t;

typedef struct {
    char    title[TEXT_TITLE];
    char    project[TEXT_PROJECT];
    char    elapsed[TEXT_ELAPSED];  // 服务端算好的成品串，e 缺失时的回退
    int     age_s;          // 服务端拍下这一行时，距最近活动多少秒
    uint8_t waiting;        // 1 = 等待操作，行首黄灯闪
} session_t;

typedef struct {
    bool      ok;
    char      error[64];
    char      clock[8];
    char      date[8];
    // 数据有多新（秒），服务端 /status 与 /screen 都下发。
    // -1 = 服务端没给（旧版服务或取不到采集时间），页脚显示 "--"。
    // 这不是 clock 那种"服务端此刻几点"，而是生成的快照距现在多久 ——
    // 只有这个数才能回答"屏幕上的数是不是新鲜的"。
    int       age;
    int       working;
    int       today_done;
    int       automations;
    int       cpu;          // %，-1 = 服务端没给该字段（旧版服务）
    int       mem;          // %
    int       disk;         // %
    int       temp;         // °C，0 = 取不到（Windows 上通常如此）
    // 芯片结温（°C），服务端从 /device/report 透传下来，-1 = 设备还没报过。
    // 这是**本机**的温度传感器读数，不是 PC 的 —— 和上面 temp 完全两回事。
    int       chip_temp;
    session_t sessions[MAX_SESSIONS];
    int       session_count;
    // ---- 详情屏 ----
    char      page_title[24];
    row_t     rows[MAX_ROWS];
    int       row_count;
    // ---- 图表屏（服务端 kind="chart" 时才非空）----
    // rows 与 charts 并存：旧服务端只发 rows，新服务端两者都发。
    // 固件优先 charts，charts 为空就退回文本行，两边都能显示。
    chart_t   charts[MAX_CHARTS];
    int       chart_count;
    bool      is_chart;
    char      foot[2][24];  // 图表屏底部两行成品文本（进程 / 运行时长）
    int       foot_count;
    int       nscr;         // 服务端下发的屏数（含概览屏）
    int       page;         // 这批数据属于第几屏，用于丢弃"切页前发出的请求"的回包
    // 本地单调时钟在这批数据被解析时的读数（ms）。会话计时靠它做锚点：
    // 屏幕上的"多久之前" = age_s + (现在 - sample_ms)/1000，所以秒针能一秒一跳，
    // 不受 5 秒轮询节奏约束。拉取失败沿用旧数据时这个值原样保留，计时继续走。
    int64_t   sample_ms;
} board_data_t;

static board_data_t       s_data;
static SemaphoreHandle_t  s_data_lock;

// ===========================================================================
// LVGL 句柄
// ===========================================================================

static lv_display_t *s_disp;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t    s_panel;

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
static lv_obj_t *s_pet_img;        // 页脚右下角像素宠物
static uint8_t   s_pet_state = PET_ST_IDLE;   // 当前宠物状态
static uint8_t   s_pet_frame = 0;             // 当前帧号（0/1）
static lv_timer_t *s_pet_timer;     // 换帧定时器，切状态时要改周期

// ---- 电量圆圈（页头右上角）----
// 纯圆弧表示进度，**圆心不放百分比数字**。
// 数字让位给充电标记：充电时圆心显示 "+"，放电时圆心为空。
static lv_obj_t *s_arc_bat;        // 进度弧
static lv_obj_t *s_lbl_chrg;       // 圆心：充电时的 "+"，放电时为空

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
static int          s_nscr    = 1; // 屏数，由服务端下发
static TaskHandle_t s_fetch_task = NULL;

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

// 按键写入的配网请求，由 LVGL 任务消费（wifi_mgr_start_provision 里有
// esp_wifi_set_mode / httpd_start，放按键任务里跑会长时间阻塞按键扫描）
static volatile bool s_prov_req = false;

// 按键写入的"重开服务地址配置页"请求，同样由 LVGL 任务执行
// （开 httpd 会阻塞几百毫秒，放按键任务里长按判定会失灵）
static volatile bool s_svrcfg_req = false;

// 标题只在内容变化时才 set_text —— 每次重设都会把跑马灯位置打回开头，
// 服务端 5 秒推一轮，不判等的话滚动会一直"闪回"。
static char     s_title_cache[MAX_SESSIONS][48];
static char     s_meta_cache[MAX_SESSIONS][64];
// 页脚右侧"多久之前"的显示缓存。ui_render 每帧都跑（约 50ms 一轮），
// 不判等就每帧 set_text 一次：每次都 malloc/free + 全量重绘，白烧 SPI。
static char     s_footer_cache[24];
// 最近一次读到的芯片结温（°C），-1 = 传感器不可用。
// 由 fetch 任务的 device_report() 刷新；UI 只读不调传感器
// （避免两个任务同时操作同一个句柄）。
// 声明放在这里而不是传感器那块：ui_render 在文件中段就要用它。
static volatile int s_chip_temp = -1;
static bool     s_waiting[MAX_SESSIONS];     // 当前哪些行在等待操作
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
// pet_update_state() 里有一模一样的守卫，原因相同。
//
// 退出：下一轮 /status 拿到 working>0 或有会话，自动恢复。
// 用户明确要"自动唤醒"，所以不绑按键 —— 按键在休眠时仍然可以切屏，
// 但切屏不会退出休眠（空闲状态下没有可看的内容）。
static bool     s_asleep      = false;  // 当前是否处于休眠屏
static lv_obj_t *s_pg_sleep;            // 休眠屏容器（全屏不透明）
static lv_obj_t *s_lbl_bigclock;        // 休眠屏：72px 大时钟
static lv_obj_t *s_lbl_sleep_dot;       // 休眠屏：在线状态点
// 休眠时背光降到这个值。30% 是"看得见但不刺眼"的下限 ——
// 再低（20%）在白天室内就快读不出时间了。
// 注意这不进 k_bright_steps：那是用户按键调节的档位，
// 休眠亮度是系统行为，不该出现在"☀ 40%"的提示里，也不该被按键改掉。
#define SLEEP_BRIGHT_PCT   30

// 按键唤醒后的保活时长。
//
// 为什么需要它：休眠是"无任务"驱动的，而用户按键去看屏的时候
// 机器多半还是无任务的 —— 没有保活的话，按下键刚亮一帧，
// 下一轮 ui_render 又判成空闲、立刻睡回去，看着像按键坏了。
// 给 60 秒：够翻完几屏、看清数字，之后自动回休眠，不用记得关。
//
// 单独一个时间戳而不是改成状态机：状态机的"醒着但空闲"和"醒着有任务"
// 在渲染上完全一样，多一个状态只会多一处要维护的分支。
#define SLEEP_GRACE_MS     (60 * 1000)
static volatile uint32_t s_awake_until_ms = 0;
// 按键任务写入的"请唤醒"请求，由 LVGL 任务消费（不能跨任务碰 LVGL）。
// 用独立标志而不是直接写 s_awake_until_ms：后者是纯数据，
// 让按键任务直接改时间戳虽然也无害，但所有 UI 相关动作集中在
// LVGL 任务里落实更好排查。
static volatile bool s_wake_req = false;

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
// 屏幕
// ===========================================================================

// 40 行缓冲 x2，内部 DMA RAM。240*40*2 = 19.2KB x2 = 38.4KB
#define LCD_BUF_LINES 40
static void *s_buf1;
static void *s_buf2;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num       = LEDC_TIMER_1;
    t.freq_hz         = 5000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {};
    c.gpio_num   = BOARD_LCD_BL;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_1;
    c.timer_sel  = LEDC_TIMER_1;
    c.duty       = 0;
    c.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void backlight_set(uint32_t percent)
{
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void display_init(void)
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = BOARD_LCD_SCLK;
    bus.mosi_io_num     = BOARD_LCD_MOSI;
    bus.miso_io_num     = -1;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = BOARD_LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = BOARD_LCD_CS;
    io_cfg.dc_gpio_num       = BOARD_LCD_DC;
    io_cfg.spi_mode          = 3;     // 与官方 board config 一致
    io_cfg.pclk_hz           = BOARD_LCD_PIXEL_CLK;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                             &io_cfg, &s_panel_io));

    // LVGL 显示对象要先建，回调里要用到
    s_disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);

    size_t buf_bytes = BOARD_LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t);
    s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "帧缓冲分配失败");
        abort();
    }
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 关键：ST7789 走 SPI 时按高字节优先接收 RGB565，而 LVGL 的 RGB565
    // 在内存里是小端（低字节在前）。不做字节交换的话：
    //   纯白 0xFFFF / 纯黑 0x0000 交换后还是自己 → 整体看着"正常"；
    //   但抗锯齿文字的中间灰度（如 0x8000 <-> 0x0080）会彻底错乱，
    //   沿字边冒出彩色噪点，看起来就是"文字花屏"。
    // LVGL 9 没有 lv_draw_sw_rgb565_swap()，正确做法是把显示层
    // 色彩格式设为 RGB565_SWAPPED，渲染时直接输出交换后的字节序，
    // 零额外遍历开销（需 CONFIG_LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=y）。
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    // 注意别加 const —— 声明成 const 再赋值会报
    // "assignment of member ... in read-only object"
    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = on_color_trans_done;
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, s_disp));

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = BOARD_LCD_RST;
    pcfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    pcfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_panel_io, &pcfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    // 这块屏必须反色（官方 board 代码同为 true）
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
}

// ===========================================================================
// UI 布局（240x240 精算）
//
//   y=0    09:44        10-26  ◕     28px 时钟行（右侧小圆弧=电量，无数字）
//   y=34   ─────────────────────     1px 分隔
//   y=40   [ 1 ] [ 1 ] [ 5 ]        46px 三格统计
//   y=92   ● 会话标题 1              40px
//          project · 5m
//   y=132  ● 会话标题 2
//   y=172  ● 会话标题 3
//   y=216  ─────────────────────
//   y=220  更新 09:44:12            20px 页脚
// ===========================================================================

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
// 充电时圆心里一个绿色 "+"，不充电时什么都不显示。
// 比原来"圈外右上角挂个 +"更好读：圈外那个小符号在缩小到 24px 后
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

// ---- 像素宠物：浮动在右下角 ----
// 素材是 PET_IMG_W x PET_IMG_H = 120x96（不是 120x120 —— 之前这里的注释
// 写错了，描述符实际 .h=96）。BOTTOM_RIGHT 对齐 + 偏移 (4, 4)：
//   对象右下角对齐屏幕右下角，再往右下各挪 4px
//   => 占 x=124..243、y=148..243
// 屏幕可见区是 0..239，所以右侧 4px、底部 4px 出屏被裁。
// 故意让它出屏一点：像素画的边缘是硬直边，正好压在屏幕边框上会像
// "贴了张方块贴纸"；溢出一点被裁掉后，视觉上更像浮在角落。
// 底部裁掉的是头盔最下沿那 1 行暗边（逻辑行 30），不影响观感。
#define PET_X           (4)
#define PET_Y           (4)
// 半透明。实测数据（sRGB 线性化 + WCAG 公式）：
//   LV_OPA_30(29.8%) -> 白头盔上屏 #4B4D52，与底色 #06080C 对比 2.37:1
//                       低于 WCAG 非文本图形 3:1 门槛，猫脸糊成一团
//   LV_OPA_40(40.0%) -> 上屏 #62656A，3.43:1 过线；压在猫身上的
//                       COL_DIM(#B4BEC9) 文字仍有 3.11:1，勉强够用
//   LV_OPA_45(45.1%) -> 猫更清楚，但文字掉到 2.60:1，会话列表开始糊
// 所以 40% 是这个布局的实际拐点：再高就伤到要读的文字了。
// 注意这里混的是屏幕背景色，不是上面的文字 —— 宠物在最底层，
// 文字画在它上面，但页脚/会话用的是灰色(COL_DIM)，压在白色猫身上
// 对比度天生不足，所以只能把猫本身压淡来让文字浮出来。
// LV_OPA_40 = 102（范围 0..255）。
#define PET_OPA         LV_OPA_40
// 页脚右侧"★ 时间"。上一版为了给页脚里 20px 的宠物让位，这里改成过 -32；
// 宠物现已独立浮在右下角（不占页脚高度），让位不再需要，恢复 -8。
#define FOOTER_TEXT_RIGHT (-8)

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

// 黄灯相位翻转。只在 LVGL 任务里（lv_timer_handler）跑，和 ui_render 同线程，
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
// 像素宠物：状态机 + 换帧
// ===========================================================================
// 四种状态对应四种"心情"，换帧周期也不同 —— 越忙动得越快，静止时慢慢眨眼，
// 一眼就能看出机器在干什么，不用去读文字。
static const struct {
    uint16_t period_ms;
    const char *name;
} PET_CFG[PET_STATES] = {
    [PET_ST_IDLE]    = { 1600, "idle"    },   // 没人用：慢眨眼，像在喘气
    [PET_ST_WORKING] = {  500, "working" },   // 干活中：眯眼盯屏，动得快
    [PET_ST_DONE]    = {  600, "done"    },   // 刚完成：张口笑，一开一合
    [PET_ST_AUTO]    = {  800, "auto"    },   // 自动化：耳朵被压，中速
};

static void pet_apply_frame(void)
{
    if (!s_pet_img) return;
    // 隐藏时不换帧：lv_image_set_src 会触发重绘，图表屏上宠物不可见，
    // 白刷一次 SPI 没有任何意义。
    if (lv_obj_has_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN)) return;
    // 帧表排布 pet_frames[state * PET_FRAMES_EACH + frame]
    const lv_image_dsc_t *dsc =
        &pet_frames[s_pet_state * PET_FRAMES_EACH + s_pet_frame];
    // 切帧 = 换指针。图像本身在 flash 里，不分配、不拷贝。
    lv_image_set_src(s_pet_img, dsc);
}

// 换帧定时器。周期跟着状态走，由 pet_set_state 负责改，这里只管翻帧。
// （LVGL 9 只有 lv_timer_set_period，没有 get_period，不要试图在这里回读校验。）
static void pet_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_pet_state >= PET_STATES) s_pet_state = PET_ST_IDLE;
    s_pet_frame ^= 1;                       // 0 <-> 1，两帧循环
    pet_apply_frame();
}

// 状态变更入口：同状态重复调用直接返回，避免每轮刷新都重设图像源。
static void pet_set_state(uint8_t st)
{
    if (st >= PET_STATES || st == s_pet_state) return;
    s_pet_state = st;
    s_pet_frame = 0;
    pet_apply_frame();

    // 周期跟着状态换，并且立刻重置计时。不 reset 的话，从 idle(1600ms)
    // 切到 working(500ms) 得等这一轮剩余的 1600ms 走完才动第一帧，
    // 用户看到的就是"有任务了但宠物愣了一秒多"。
    if (s_pet_timer) {
        lv_timer_set_period(s_pet_timer, PET_CFG[st].period_ms);
        lv_timer_reset(s_pet_timer);
    }
}

// "刚完成"高亮：today_done 增加时记一个到期时间戳，8 秒内保持 DONE 表情。
// 用时间戳而不是计数器，是为了不依赖刷新频率（HTTP 轮询间隔可能变）。
#define PET_DONE_HOLD_MS  8000
static int64_t s_done_until_ms = 0;
static int     s_prev_today_done = -1;

// 上一轮的"有没有会话在等操作"。语音提示靠它的**上升沿**触发。
// 用边沿而不是电平，因为服务端 5 秒推一轮，等待状态通常持续好几分钟 ——
// 用电平的话会每 5 秒叫一次，夜里能把人吵醒。
static bool s_prev_any_waiting = false;

// 上一轮的活跃会话数，用于检测"会话收工"（working 减少）。
//
// ⚠️ 这里有个必须处理的数据特性：working 的 SQL 是
//     LOWER(status) IN ('working','active') AND last_activity_at >= now-30min
// 也就是说会话超过 30 分钟没动静，会自动滑出计数 —— 这不是"完成"，
// 只是"过期"。如果直接对 working 减少就响铃，那么任何一次长时间离开
// 电脑再回来，都会听到莫名其妙的"完成"提示。
//
// 对策：减少时再确认一次 today_done 没有同时变化（那是真的完成 todo，
// 已经由上面分支响过 DONE 音了），并且要求减少后仍有数据、非首次同步。
// 更关键的是这里**不做任何猜测性播放**：只把减少也算作"有进展"，
// 让它和 todo 完成共用去抖窗口，短时间内只响一次，避免连响。
static int  s_prev_working = -1;

static void pet_update_state(const board_data_t *d)
{
    // 只有概览屏（第 0 屏，走 /status）的数据里带 working / today_done / sessions。
    // 详情屏（/screen/<i>）只下发 rows，这三个字段在 parse_into 里 fallback 成 0。
    //
    // 如果照旧参与判断，切一次屏就会：
    //   概览(working=2, done=3) -> 详情(0, 0)  : working 减少 → 误响"收工"
    //   详情(0, 0) -> 概览(2, 3)               : done 增加   → 误响"完成"
    // 也就是每切一圈屏白响两声。加音频之前只表现为表情闪一下，
    // 现在会变成实体噪音，所以非第 0 屏直接跳过，宠物保持原状态不动。
    if (d->page != 0) return;

    int64_t now = esp_timer_get_time() / 1000;

    // 本轮是否已经因为"todo 完成"响过。用于避免同一轮里
    // DONE 和 SESSION_END 连着响两声。
    bool played_done = false;

    // 首次拿到数据时只记基线，不触发完成动画 —— 否则每次开机都要先笑一下
    if (s_prev_today_done < 0) {
        s_prev_today_done = d->today_done;
        s_prev_working    = d->working;
        ESP_LOGI(TAG, "pet 基线: done=%d working=%d",
                 s_prev_today_done, s_prev_working);
    } else if (d->today_done > s_prev_today_done) {
        // todo 勾完一项或多项。
        // 一次完成多项（0 -> 3）也只响一次：连响三次纯属噪音，
        // "刚才有进展"这一条信息响一声就够了。
        ESP_LOGI(TAG, "pet 完成: done %d -> %d，播放完成音",
                 s_prev_today_done, d->today_done);
        s_done_until_ms = now + PET_DONE_HOLD_MS;
        s_prev_today_done = d->today_done;
        audio_play(AUDIO_EV_DONE);
        played_done = true;
    } else if (d->today_done < s_prev_today_done) {
        ESP_LOGI(TAG, "pet 基线回退: done %d -> %d",
                 s_prev_today_done, d->today_done);
        s_prev_today_done = d->today_done;   // 跨天归零，跟一下基线
    }

    // 会话收工：working 减少。
    //
    // ⚠️ working 的 SQL 带 30 分钟活跃窗口：
    //     LOWER(status) IN ('working','active') AND last_activity_at >= now-30min
    // 所以会话只是"久没动静"也会滑出计数，那不是完成。
    // 完全靠这个信号会误报，因此：
    //   - 本轮已经响过 DONE（真的完成了 todo）就不再响，避免连响两声
    //   - 用 played_done 判断而不是比较 s_prev_today_done ——
    //     后者在上一分支里已经更新过了，比较永远成立，是个隐蔽的 bug
    if (s_prev_working >= 0 && d->working < s_prev_working && !played_done) {
        audio_play(AUDIO_EV_SESSION_END);
    }
    s_prev_working = d->working;

    // 有没有会话在等用户操作 —— 比"有几个自动化"重要得多
    bool any_waiting = false;
    for (int i = 0; i < d->session_count; i++) {
        if (d->sessions[i].waiting) { any_waiting = true; break; }
    }

    // 只在"从无到有"那一刻响。持续等待期间不重复提示。
    if (any_waiting && !s_prev_any_waiting) {
        audio_play(AUDIO_EV_ATTENTION);
    }
    s_prev_any_waiting = any_waiting;

    // 优先级：刚完成 > 有会话在等 > 有任务在跑 > 待机
    //
    // 注意不能拿 d->automations 判断"自动化在跑"：
    // 那个字段是"已启用的自动化总数"，配置了就一直 >0（当前机器是 5），
    // 用它做条件等于永远进不了 IDLE，宠物会一直抖耳朵。
    // AUTO 只用于"确实没有会话、但也不该显示纯待机"的兜底，这里索性并入
    // WORKING 之外的判断 —— 让 idle 真的能出现。
    uint8_t st;
    if (now < s_done_until_ms)     st = PET_ST_DONE;
    else if (any_waiting)          st = PET_ST_AUTO;     // 耳朵压扁：等你操作
    else if (d->working > 0)   st = PET_ST_WORKING;  // 干活中
    else                           st = PET_ST_IDLE;

    pet_set_state(st);
}

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

// ---- 休眠屏 ----
// 空转时屏幕上只剩"几点 + 还在线"，其余全藏掉。
//
// 布局（240x240）：
//        ┌──────────────────┐
//        │                  │
//        │      14:43       │  ← 72px 大时钟，180x52，居中后占 y=70..122
//        │                  │
//        │      ● 在线      │  ← 12px，y=150
//        └──────────────────┘
//
// 为什么时钟不放正中：正中心 y=120 时，下面 28px 处才是状态点，
// 两点挤在一起像图标的副标题。把整组当成一个整体、视觉重心上移一点
// （时钟中心落在 y=96，约屏幕 40% 处）更接近"时钟 App"的观感。
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

    // 大时钟。用 TOP_MID + 固定 y 定位而不是 CENTER：
    // 字体 line_height=52，CENTER 时"盒中心"对屏幕中心，看着偏高
    // （因为数字的墨迹集中在盒子上部，base_line=0）。y=70 让墨迹
    // 落在视觉中心附近。
    s_lbl_bigclock = mk_label(s_pg_sleep, &font_cn_72, COL_TEXT,
                              LV_ALIGN_TOP_MID, 0, 70);
    lv_label_set_text(s_lbl_bigclock, "--:--");

    // 在线状态点。只在休眠屏出现，和页头那个"● 在线"是两份独立控件 ——
    // 复用同一个标签就得在休眠/唤醒时改名、改字体、改坐标，反而更容易错。
    // 一个 12px 标签的 RAM 代价可以忽略。
    s_lbl_sleep_dot = mk_label(s_pg_sleep, &font_cn_12, COL_OK,
                               LV_ALIGN_TOP_MID, 0, 150);
    lv_label_set_text(s_lbl_sleep_dot, "● 在线");
}

static void ui_build(void)
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
    // 素材是 ARGB8888（带真 alpha），所以可以整体设不透明度而不产生黑罩：
    // 透明区 alpha=0 完全透出下层，只有猫本身半透明。
    s_pet_img = lv_image_create(scr);
    lv_image_set_src(s_pet_img, &pet_frames[PET_ST_IDLE * PET_FRAMES_EACH]);

    // 不做缩放。素材本身就是 120x96，1:1 绘制。
    //
    // 上一版用 lv_image_set_scale(512) 想把 60x60 放大到 120，屏幕上只露出
    // 左上角一块。原因是 set_scale 只改"绘制变换"，对象尺寸仍是 60x60，
    // 而 pivot 默认在图像中心，放大后图像以中心向四周各溢出 30px，
    // 超出对象边界就被裁掉。改素材尺寸比改 pivot/size 组合可靠得多。

    // 半透明。注意用 image_opa 而不是 obj_set_style_opa：
    // 后者会把整个对象（含可能的子对象）整体合成一次，开销更大；
    // image_opa 直接作用在图像绘制阶段，正是我们要的效果。
    lv_obj_set_style_image_opa(s_pet_img, PET_OPA, 0);

    // 不让它拦截按键/点击，纯装饰
    lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_align(s_pet_img, LV_ALIGN_BOTTOM_RIGHT, PET_X, PET_Y);

    // 定时换帧，间隔按状态在 pet_set_state 里动态改
    s_pet_timer = lv_timer_create(pet_timer_cb, PET_CFG[PET_ST_IDLE].period_ms, NULL);

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

    // 宠物压到最底层：作为屏幕的第 0 个子对象，先于所有内容绘制。
    //
    // LVGL 没有 z-index，子对象顺序就是绘制顺序 —— index 0 最先画（最底），
    // index 越大越靠上。所以这里显式移到 0，让页头、三格统计、会话列表、
    // 页脚文字全都盖在它上面。
    //
    // 用 lv_obj_move_to_index 而不是 lv_obj_move_background：
    // 后者在 LVGL 9 里只是 api_map_v8.h 里的 static inline 兼容别名
    // （内部就是 move_to_index(obj, 0)），直接用新名字更明确。
    lv_obj_move_to_index(s_pet_img, 0);

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
}

// ===========================================================================
// 渲染
//
// ⚠ 这组函数**不持 s_data_lock**，只读传入的快照。
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
//   注意：真断网（ok=false）时 ui_render 走离线分支，页脚右侧会被清空、
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

            char meta[64];
            snprintf(meta, sizeof(meta), "%s · %s", proj, age);
            // meta 也判等，避免每轮都触发该行的重绘
            if (strcmp(meta, s_meta_cache[i]) != 0) {
                strncpy(s_meta_cache[i], meta, sizeof(s_meta_cache[i]) - 1);
                s_meta_cache[i][sizeof(s_meta_cache[i]) - 1] = '\0';
                lv_label_set_text(s_lbl_sess_meta[i], meta);
            }

            // 等待操作 → 灯变黄 + 交给 blink_timer_cb 闪；不等待就整颗隐藏，
            // 行首留 10px 白，比常亮一个灰点干净
            bool waiting = d->sessions[i].waiting != 0;
            s_waiting[i] = waiting;
            if (waiting) {
                lv_obj_set_style_bg_color(s_dot[i], COL_WARN, 0);
                lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
                lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            }

            lv_obj_clear_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            s_waiting[i] = false;
            lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (d->session_count == 0) {
        lv_label_set_text(s_lbl_sess_title[0], "空闲中");
        lv_obj_set_style_text_color(s_lbl_sess_title[0], COL_DIM, 0);
        lv_obj_clear_flag(s_lbl_sess_title[0], LV_OBJ_FLAG_HIDDEN);
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
        // ui_render 每帧都跑，不判等会一直触发重绘把 SPI 占满。
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
// 和列表屏同一个理由：ui_render 每帧都跑，每次都 set_value/set_text 会一直
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
// 背光直接调 backlight_set()，**不碰 s_bright_idx**：
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
        lv_obj_add_flag(s_pet_img,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_main,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_list,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_chart,   LV_OBJ_FLAG_HIDDEN);

        // logo 是个局部变量，没存句柄。它被 s_pg_sleep 的纯黑底盖住，
        // 不需要单独藏 —— 这也是给容器铺不透明底的主要原因之一。

        lv_obj_clear_flag(s_pg_sleep, LV_OBJ_FLAG_HIDDEN);
        backlight_set(SLEEP_BRIGHT_PCT);
        ESP_LOGI(TAG, ">>> 进入休眠屏（无任务，背光 %d%%）", SLEEP_BRIGHT_PCT);
    } else {
        // ---- 唤醒：把可见性交还给 page_apply ----
        //
        // 不在这里逐个 clear 页头控件，而是调用 page_apply 的逻辑：
        // 它会根据 s_page 决定该亮哪个容器。页头那几个控件反正一直可见，
        // 这里直接全部 clear 即可（page_apply 不碰页头）。
        lv_obj_add_flag(s_pg_sleep, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_clock,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_date,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_arc_bat,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_chrg,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_sys,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_footer, LV_OBJ_FLAG_HIDDEN);

        // 页面容器按 s_page 恢复，和 page_apply 的规则保持一致
        if (s_page == 0) {
            lv_obj_clear_flag(s_pg_main, LV_OBJ_FLAG_HIDDEN);
        } else if (s_page < MAX_PAGES && s_page_is_chart[s_page]) {
            lv_obj_clear_flag(s_pg_chart, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
        }

        // 宠物：图表屏仍然要藏（pet_should_show 的规则）
        if (s_pet_img && pet_should_show()) {
            lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN);
            pet_apply_frame();
        }

        // 背光恢复成用户调的档位
        backlight_set(k_bright_steps[s_bright_idx]);
        ESP_LOGI(TAG, ">>> 退出休眠屏（有任务，背光 %u%%）",
                 k_bright_steps[s_bright_idx]);
    }
}

// 根据最新数据决定该不该休眠。每轮 ui_render 都调用，内部有状态判等，
// 稳态下只是一次比较。
//
// ⚠ d->page == 0 这个守卫不能省，理由见 s_asleep 的注释。
// 注意同时要求 s_page == 0：数据属于第 0 屏、但用户已经按键切走了的话，
// 屏幕上是详情屏，这时候休眠会把人正在看的内容藏掉。
static void sleep_update(const board_data_t *d)
{
    if (!d->ok) {
        // 离线 / 服务端降级：这是需要被看到的异常，不休眠。
        // 如果已经在休眠就从里面出来 —— 否则屏幕停在一块"● 在线"上，
        // 而实际上早就断了，是最糟的误导。
        if (s_asleep) sleep_apply(false);
        return;
    }

    bool idle = (d->working == 0) && (d->session_count == 0);
    bool on_overview = (d->page == 0) && (s_page == 0);

    // 按键唤醒后的保活期内不休眠，否则按下去只亮一帧就黑回去。
    if (s_awake_until_ms) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        // 用有符号差判断：esp_timer 是 64 位微秒，取毫秒后还是 64 位，
        // 但存进 uint32 会 49 天回绕。有符号比较能正确处理回绕。
        if ((int32_t)(s_awake_until_ms - now) > 0) {
            on_overview = false;         // 保活期内：当作"不该休眠"
        } else {
            s_awake_until_ms = 0;        // 到期，清掉，恢复按空闲判定
        }
    }

    sleep_apply(idle && on_overview);
}

static void page_apply(int page)
{
    if (s_nscr < 1) s_nscr = 1;
    page = ((page % s_nscr) + s_nscr) % s_nscr;   // 环形，负数也安全
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
        if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
        ESP_LOGI(TAG, ">>> 休眠中切屏 -> %d/%d（暂不显示）", page, s_nscr);
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
        // 数据一到，ui_render 发现 kind=chart 会立刻切到图表容器。
        ui_list_reset();
        lv_obj_clear_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
    }

    // 切屏后立刻同步宠物的可见性。图表屏藏宠物，避免它压住环下标签。
    if (s_pet_img) {
        if (pet_should_show()) {
            lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN);
            // 图表屏期间 pet_apply_frame 是跳过的（省 SPI），切回来时
            // 图像源可能停在隐藏前的那一帧。补一次刷新，让表情立刻对上
            // 当前状态，而不是等下一个换帧周期。
            pet_apply_frame();
        } else {
            lv_obj_add_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 叫醒拉取任务立刻换 URL 重拉，不等下一个 5 秒周期
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
    ESP_LOGI(TAG, ">>> 切屏 -> %d/%d", page, s_nscr);
}

// 电量圆圈渲染。只在 LVGL 任务里调用（和 ui_render 同线程）。
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

        // 充电标记。字体没有 ⚡，用 "+"。
        // 放在圆心（原数字的位置）。放电时清空。
        // 颜色跟随充电态：固定绿色，和进度弧一致。
        if (b.charging) {
            lv_obj_set_style_text_color(s_lbl_chrg, COL_OK, 0);
            lv_label_set_text(s_lbl_chrg, "+");
        } else {
            lv_label_set_text(s_lbl_chrg, "");
        }
    }
}

static void ui_render(const board_data_t *d)
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
        // 不加这个判等的话，5 秒一轮的 ui_render 会每分钟白白重绘 12 次，
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
        pet_set_state(PET_ST_IDLE);
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
        // 走一趟链路最坏要 35 秒。而 s_chip_temp 是 device_report() 里
        // 刚读到的值，屏幕 50ms 一刷就能跟上，反应快得多。
        // d->chip_temp 的意义是"服务端也拿到了这份数据"，供别的端看。
        int t = (s_chip_temp >= 0) ? s_chip_temp : d->chip_temp;
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
    pet_update_state(d);

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
        if (s_pet_img) {
            if (pet_should_show()) {
                lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN);
                pet_apply_frame();
            } else {
                lv_obj_add_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_page == 0)                                        ui_render_overview(d);
    else if (s_page < MAX_PAGES && s_page_is_chart[s_page]) ui_render_chart(d);
    else                                                    ui_render_list(d);
}

// ===========================================================================
// 极简 JSON 解析
// 输出格式由我们自己的 bridge 完全控制，不需要通用解析器
// ===========================================================================

static bool json_str(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            out[i++] = (*p == 'n' || *p == 't') ? ' ' : *p;
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return true;
}

static int json_int(const char *json, const char *key, int fallback)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return fallback;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p == '"') return fallback;
    return atoi(p);
}

// 定位 sessions 数组的第 n 个对象，返回起止指针
static bool json_nth_object(const char *json, const char *array_key, int n,
                            const char **start, const char **end)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":[", array_key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);

    int depth = 0, idx = -1;
    const char *obj_start = NULL;
    /* ⚠ 必须区分字符串内外。
     * 会话标题是 AI 生成的文本，里面出现 { } ] 都很正常
     * （"fix {bug}"、"数组越界 [i]"）。
     * 原实现只看字符本身，于是：
     *   - 标题里有 { → depth 提前归零 → end 落在标题中间，
     *     后面的 project/elapsed 全解析成空；
     *   - 标题里有 ] → 直接判定"数组结束"，整行会话凭空消失。
     * 字符串内的括号是文本，不参与结构计数。 */
    bool in_str = false, esc = false;

    while (*p) {
        char c = *p;

        if (in_str) {
            /* 字符串内部：只有 \" 是转义，其它字符都是文本 */
            if (esc)          esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  in_str = false;
            p++;
            continue;
        }

        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            if (depth == 0) {
                idx++;
                if (idx == n) obj_start = p;
            }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && obj_start) {
                *start = obj_start;
                *end   = p;
                return true;
            }
        } else if (c == ']' && depth == 0) {
            return false;
        }
        p++;
    }
    return false;
}

static void parse_into(board_data_t *d, const char *json)
{
    memset(d, 0, sizeof(*d));
    d->ok = (strstr(json, "\"ok\":true") != NULL);
    if (!d->ok) json_str(json, "error", d->error, sizeof(d->error));

    json_str(json, "clock", d->clock, sizeof(d->clock));
    json_str(json, "date",  d->date,  sizeof(d->date));
    // 缺省 -1 表示"服务端没下发"，页脚显示 --，而不是假装"刚刚更新"
    d->age = json_int(json, "age", -1);
    d->working     = json_int(json, "working", 0);
    d->today_done  = json_int(json, "today_done", 0);
    d->automations = json_int(json, "automations", 0);
    // 缺省 -1 表示"服务端没下发"，页脚显示 --，不与真实的 0% 混为一谈
    d->cpu  = json_int(json, "cpu",  -1);
    d->mem  = json_int(json, "mem",  -1);
    d->disk = json_int(json, "disk", -1);
    d->temp = json_int(json, "temp",  0);
    // 芯片结温：没报过服务端给 -1，页脚显示 "--"，不显示假 0°C
    d->chip_temp = json_int(json, "chip_temp", -1);
    // 屏数：/status 和 /screen 都下发 nscr，固件据此决定按几屏循环
    d->nscr = json_int(json, "nscr", 0);

    // ---- 详情屏：页标题 + rows 数组 ----
    json_str(json, "t", d->page_title, sizeof(d->page_title));
    for (int i = 0; i < MAX_ROWS; i++) {
        const char *start = NULL, *end = NULL;
        if (!json_nth_object(json, "rows", i, &start, &end)) break;

        /* ⚠ 必须和 sessions 那段一样用 512。
         * 原来这里是 256，同一份数据在 1514 行用 sub[512]。当前字段量
         * （40+20+12 字节中文标题）撑不到 256，但服务端一加字段就静默截断 ——
         * 截断后 json_str 找不到键 → 显示空白，不崩、不报错，很难发现。 */
        char sub[512];
        size_t len = (size_t)(end - start) + 1;
        if (len >= sizeof(sub)) len = sizeof(sub) - 1;
        memcpy(sub, start, len);
        sub[len] = '\0';

        json_str(sub, "t", d->rows[i].t, sizeof(d->rows[i].t));
        json_str(sub, "v", d->rows[i].v, sizeof(d->rows[i].v));
        int c = json_int(sub, "c", 0);
        d->rows[i].color = (uint8_t)(c < 0 ? 0 : (c > 4 ? 4 : c));
        d->row_count++;
    }

    // ---- 图表屏：kind + charts 数组 + 底部两行 ----
    // kind 缺失 = 老服务端，is_chart 保持 false，走文本渲染，不会白屏。
    char kind[8];
    d->is_chart = json_str(json, "kind", kind, sizeof(kind))
                  && strcmp(kind, "chart") == 0;
    if (d->is_chart) {
        for (int i = 0; i < MAX_CHARTS; i++) {
            const char *start = NULL, *end = NULL;
            if (!json_nth_object(json, "charts", i, &start, &end)) break;

            // charts 对象比 rows 小得多（约 40 字节），128 足够。
            // 故意不用 512：这个数组每屏最多 3 个，省栈。
            char sub[128];
            size_t len = (size_t)(end - start) + 1;
            if (len >= sizeof(sub)) len = sizeof(sub) - 1;
            memcpy(sub, start, len);
            sub[len] = '\0';

            json_str(sub, "k", d->charts[i].k, sizeof(d->charts[i].k));
            json_str(sub, "n", d->charts[i].n, sizeof(d->charts[i].n));
            int v = json_int(sub, "v", 0);
            d->charts[i].v = (v < 0) ? 0 : (v > 100 ? 100 : v);
            int c = json_int(sub, "c", 0);
            d->charts[i].color = (uint8_t)(c < 0 ? 0 : (c > 5 ? 5 : c));
            d->chart_count++;
        }
        json_str(json, "f1", d->foot[0], sizeof(d->foot[0]));
        json_str(json, "f2", d->foot[1], sizeof(d->foot[1]));
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        const char *start = NULL, *end = NULL;
        if (!json_nth_object(json, "sessions", i, &start, &end)) break;

        char sub[512];
        size_t len = (size_t)(end - start) + 1;
        if (len >= sizeof(sub)) len = sizeof(sub) - 1;
        memcpy(sub, start, len);
        sub[len] = '\0';

        json_str(sub, "title",   d->sessions[i].title,   sizeof(d->sessions[i].title));
        json_str(sub, "project", d->sessions[i].project, sizeof(d->sessions[i].project));
        json_str(sub, "elapsed", d->sessions[i].elapsed, sizeof(d->sessions[i].elapsed));
        // e = 距最近活动的原始秒数，固件据此本地每秒累加。
        // 缺省 -1 表示"服务端没发"，此时回退到 elapsed 那个成品串，
        // 与真实的 0s（刚刚活动过）区分开
        int e = json_int(sub, "e", -1);
        d->sessions[i].age_s = (e < 0) ? -1 : e;
        // w 只在等待操作时下发，缺省即 0
        d->sessions[i].waiting = (uint8_t)json_int(sub, "w", 0);
        d->session_count++;
    }

    // 计时锚点：这批数据被解析时的本地时刻。会话年龄是相对服务端 now 的，
    // 加上从这一刻起流逝的时间 = 此刻的年龄。放在 memset 之后设，否则会被清掉。
    d->sample_ms = esp_timer_get_time() / 1000;
}

// ===========================================================================
// HTTP 轮询
// ===========================================================================

#define HTTP_BUF_SIZE 4096
static char s_http_buf[HTTP_BUF_SIZE];

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} accum_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    accum_t *a = (accum_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && a) {
        size_t space = a->cap - a->len - 1;
        size_t copy  = (evt->data_len < (int)space) ? (size_t)evt->data_len : space;
        if (copy) {
            memcpy(a->buf + a->len, evt->data, copy);
            a->len += copy;
            a->buf[a->len] = '\0';
        }
    }
    return ESP_OK;
}

// ===========================================================================
// 芯片温度 + 设备上报
// ===========================================================================
// 温度传感器只在启动时装一次，句柄常驻。
// ESP32-S3 内置的其实是**射频用的**温度传感器，精度 ±1°C 左右，
// 测的是芯片结温（die temperature），不是环境温度 ——
// 芯片自己发热，读数天然比室温高 5~15°C，满载时更高。
// 所以它适合回答"芯片有没有过热"，不能当室温计用。
static temperature_sensor_handle_t s_tsens = NULL;

static void tsens_init(void)
{
    // 范围给 -10~80：芯片结温正常在 30~70，超出这个范围要么是没焊好，
    // 要么是真的过热，两种情况都该被看到而不是被钳在边界上。
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "温度传感器不可用: %s（页脚将显示 --）", esp_err_to_name(err));
        s_tsens = NULL;
        return;
    }
    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "温度传感器使能失败: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        return;
    }
    ESP_LOGI(TAG, "温度传感器就绪");
}

static int tsens_read(void)
{
    if (!s_tsens) return -1;
    float c = 0;
    if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) return -1;
    return (int)(c + (c >= 0 ? 0.5f : -0.5f));   // 四舍五入到整数
}

// 把板载指标 POST 给服务端。
//
// 为什么用 POST 而不是塞进 GET 的 query：
//   GET /status 是**服务端 → 设备**的通道，且被标记为冻结契约。
//   上报是反方向的，混在一起会让"设备只读不写"和"设备只写不读"
//   两种用法互相牵连。独立端点出问题时也不会影响看板刷新。
//
// 为什么失败不重试：
//   30 秒后又报一次，重试的边际价值几乎为零，反而可能拖住 fetch 任务。
//   温度不是告警数据，晚一轮没有后果。
#define DEVICE_REPORT_INTERVAL_MS  (30 * 1000)

static void device_report(void)
{
    const char *base = wifi_mgr_svr_base();
    if (!base || !base[0]) return;

    int t = tsens_read();
    s_chip_temp = t;

    battery_state_t b;
    battery_get(&b);

    // JSON 手工拼：只有 4 个字段，上 cJSON 要多分配一次堆，
    // 而这函数在内存紧张的看板固件里每 30 秒跑一次，能省就省。
    char body[128];
    snprintf(body, sizeof(body),
             "{\"chip_temp\":%d,\"battery\":%d,\"charging\":%s,\"uptime_s\":%lld}",
             t < 0 ? 0 : t,              // 传感器坏了就先报 0，服务端另有 -1 语义
             b.percent,
             b.charging ? "true" : "false",
             (long long)(esp_timer_get_time() / 1000000));

    char url[96];
    snprintf(url, sizeof(url), "%s/device/report", base);

    esp_http_client_config_t cfg = {};
    cfg.url        = url;
    cfg.method     = HTTP_METHOD_POST;
    cfg.timeout_ms = 4000;          // 比 GET 短：上报失败无所谓，别拖时间

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return;

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(cli);
    int code = (err == ESP_OK) ? esp_http_client_get_status_code(cli) : 0;
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || code / 100 != 2) {
        // 用 DEBUG 级别：这个失败不影响看板，刷 WARN 只会淹没真正的问题
        ESP_LOGD(TAG, "上报失败 err=%s code=%d", esp_err_to_name(err), code);
    } else {
        ESP_LOGD(TAG, "上报 chip_temp=%d bat=%d%%", t, b.percent);
    }
}

static void fetch_task(void *arg)
{
    // 让按键能 xTaskNotifyGive 叫醒本任务，切屏后立刻重拉，不等 5 秒周期
    s_fetch_task = xTaskGetCurrentTaskHandle();

    accum_t acc = { s_http_buf, 0, HTTP_BUF_SIZE };

    esp_http_client_config_t cfg = {};
    cfg.url        = wifi_mgr_svr_base();   /* 初值；每轮循环都会按当前地址重设 */
    cfg.method     = HTTP_METHOD_GET;
    cfg.event_handler = http_event_cb;
    cfg.user_data  = &acc;
    cfg.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP 初始化失败");
        vTaskDelete(NULL);
        return;
    }

    int fails = 0;
    uint32_t base_ver = wifi_mgr_svr_version();

    while (1) {
        // WiFi 不通就别拉了。这时 HTTP 只会超时（一次 5 秒）并且把失败计数
        // 算到服务端头上 —— 屏幕会误弹"改服务地址"。等连上再说。
        if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
            wifi_mgr_svr_reset();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }

        // 板载指标上报（温度/电量），30 秒一次。
        // 放在 GET 之前：万一 POST 卡住超时（4 秒），也只影响这一轮的上报，
        // 不会让看板数据晚到 —— 下一轮 GET 照常。
        {
            int64_t now_ms = esp_timer_get_time() / 1000;
            static int64_t last_report_ms = 0;
            if (now_ms - last_report_ms >= DEVICE_REPORT_INTERVAL_MS) {
                last_report_ms = now_ms;
                device_report();
            }
        }

        // 每轮都重新读屏号和**服务地址**：按钮随时可能切屏，配置页也随时
        // 可能改地址 —— 两者都不需要重启固件，下一轮就生效。
        int req_page = s_page;
        const char *base = wifi_mgr_svr_base();
        char url[96];
        if (req_page == 0) {
            snprintf(url, sizeof(url), "%s%s", base, BRIDGE_PATH_MAIN);
        } else {
            snprintf(url, sizeof(url), "%s/screen/%d", base, req_page);
        }
        esp_http_client_set_url(client, url);

        acc.len = 0;
        s_http_buf[0] = '\0';

        esp_err_t err = esp_http_client_perform(client);
        // perform 失败时 handle 里的 status_code 是**上一轮的残留值**，
        // 直接打印会看到 "err=ESP_ERR_HTTP_CONNECT status=200" 这种自相矛盾的日志，
        // 排查时极易误判成"服务端返回 200 但解析失败"。连接没通就没有 HTTP 状态码。
        int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;

        board_data_t parsed = {};
        parsed.page = req_page;                 // 标记这批数据属于哪一屏
        bool got = (err == ESP_OK && status == 200 && acc.len > 0);

        // 服务地址对不对是 wifi_mgr 管的账：连续拉不到就会开放配置页 +
        // 让屏幕弹提示；拉通一次就把提示收回去、页面关掉。
        wifi_mgr_svr_report(got);

        if (got) {
            parse_into(&parsed, s_http_buf);
            parsed.page = req_page;             // parse_into 里 memset 过，要重设
            fails = 0;

            // 首次拉取成功响一声"已连接"。
            // 用静态标志而不是比较 s_data.ok：后者在短暂失败时会沿用旧数据
            // 保持 true，网络抖一下就重响，很烦。这里严格只在**第一次**
            // 拿到 200 时响一次，之后无论断连多少次都不再响。
            static bool s_greeted = false;
            if (!s_greeted) {
                s_greeted = true;
                audio_play(AUDIO_EV_CONNECTED);
            }

            // 屏数由服务端说了算，改屏数不用重烧固件
            if (parsed.nscr > 0) s_nscr = parsed.nscr;
            ESP_LOGI(TAG, "OK p=%d n=%d working=%d done=%d auto=%d sess=%d rows=%d btn=%s",
                     req_page, s_nscr, parsed.working, parsed.today_done,
                     parsed.automations, parsed.session_count, parsed.row_count,
                     (const char *)s_last_btn);
        } else {
            fails++;
            ESP_LOGW(TAG, "拉取失败 err=%s status=%d (连续%d次)",
                     esp_err_to_name(err), status, fails);
            parsed.ok = false;
            // status==0 表示没拿到 HTTP 响应（连不上/超时），这时写 "HTTP 0"
            // 会让人以为是服务端返回了非法状态码，其实压根没连上。
            if (status > 0) snprintf(parsed.error, sizeof(parsed.error), "HTTP %d", status);
            else            snprintf(parsed.error, sizeof(parsed.error), "%.40s",
                                     esp_err_to_name(err));
            parsed.page = req_page;

            // 短暂失败沿用旧数据，只把状态点变红。
            // sample_ms 跟着旧数据一起留下来 —— 计时锚点不变，
            // 断网期间秒针照常走，比冻在最后一格诚实
            if (xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
                bool had = s_data.ok;
                if (had) {
                    int keep = parsed.page;
                    parsed = s_data;
                    parsed.page = keep;
                }
                parsed.ok = false;
                xSemaphoreGive(s_data_lock);
            }
        }

        if (xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_data = parsed;
            xSemaphoreGive(s_data_lock);
        }

        // 用通知代替死等：切屏时 xTaskNotifyGive 会让它立刻醒来换 URL
        uint32_t wait = (fails > 3) ? 15000 : POLL_INTERVAL_MS;
        // 服务地址刚被改过：别等这一轮走完，300ms 后立刻按新地址重试，
        // 否则用户改完地址要干等十几秒才看到屏幕提示消失。
        uint32_t ver = wifi_mgr_svr_version();
        if (ver != base_ver) {
            base_ver = ver;
            wait = 300;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
}

// ===========================================================================
// WiFi
//
// 连接、重试、配网全部收敛到 wifi_mgr.cpp。这里只保留两件事：
//   · 启动时调一次 wifi_mgr_begin()
//   · 屏幕按 wifi_mgr_state() 显示对应提示
//
// 为什么从本文件搬走：原来 wifi_handler 在 STA_DISCONNECTED 回调里
// vTaskDelay(2000) 再重连，会把默认事件循环任务卡住（单线程），
// 期间 WiFi/IP 的所有事件都出不来。新实现改用 esp_timer 调度。
// ===========================================================================

/* ⚠ 这里原来配 SNTP + TZ，是一段死代码，已删除。
 *
 * 屏幕上的 clock / date 全部来自服务端（api_server.py 里 datetime.now()
 * 格式化后下发），固件从头到尾没有调用过 localtime / strftime。
 * 也就是说：SNTP 对齐的时间戳，**没有任何代码去读**。
 *
 * 留着它的代价是实打实的：
 *   · esp_sntp_init() 会持续向公网发 NTP 请求，流量和功耗都是白付；
 *   · 两台服务器轮询，重连 WiFi 后还会重新握手；
 *   · 设备在断开 bridge、根本没网的时候也在对外面发包。
 *
 * 真要用本地时间（比如离线时显示时间），正确做法是加一个
 * localtime() + strftime() 的渲染路径，再把 SNTP 调回来。 */
#ifdef CONFIG_WB_LOCAL_TIME
static void time_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_init();
}
#endif

// ===========================================================================
// 按键：先当"探针"用 —— 三个候选引脚全监听，谁被按下日志里就报谁
// ===========================================================================

typedef struct {
    gpio_num_t  pin;
    const char *name;
} btn_cfg_t;

static const btn_cfg_t k_btns[] = {
    { BOARD_BTN_BOOT,     "BOOT(GPIO0)"     },
    { BOARD_BTN_VOL_UP,   "VOLUP(GPIO40)"   },
    { BOARD_BTN_VOL_DOWN, "VOLDN(GPIO39)"   },
};
#define BTN_NUM (sizeof(k_btns) / sizeof(k_btns[0]))

static void button_init(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < BTN_NUM; i++) {
        mask |= 1ULL << k_btns[i].pin;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = mask;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;   // 按下接地 → 低电平
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;    // 用轮询，抖动好处理
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_LOGI(TAG, "按键监听: BOOT=GPIO%d VOLUP=GPIO%d VOLDN=GPIO%d",
             BOARD_BTN_BOOT, BOARD_BTN_VOL_UP, BOARD_BTN_VOL_DOWN);
}

// step: 0 = 循环（亮→暗→亮），正数 = 调亮，负数 = 调暗
static void button_action(const char *who, int step)
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

static void button_task(void *arg)
{
    // 消抖：按下要连续 NEED 次低电平确认；松开后必须稳定 REL 次才接受下一次按下，
    // 否则引脚抖动会把一次按压刷成好几次，亮度乱跳。
    const int NEED = 5;    // 20ms * 5  = 100ms 按下确认
    const int REL  = 10;   // 20ms * 10 = 200ms 松开确认
    // 短按 = 切屏，长按 = 调亮度。按下那一刻不知道用户要干嘛，
    // 所以动作只能等到"按住够久"或"松开"时才分派。
    const uint32_t LONG_MS = 700;

    // WiFi 没连上时，BOOT 的长按改语义：按住 3 秒进配网。
    // 这个状态下**不触发** 700ms 的亮度循环 —— 否则一次按住会先跳一档亮度
    // 再进配网，用户会困惑"我明明只想配网，怎么屏幕变亮了"。
    const uint32_t PROV_MS = 3000;

    bool     last[BTN_NUM];
    int      cnt[BTN_NUM]     = {0};
    int      rel[BTN_NUM];
    uint32_t down_at[BTN_NUM] = {0};
    bool     fired[BTN_NUM]   = {false};   // 本次按压是否已按长按处理过

    for (size_t i = 0; i < BTN_NUM; i++) {
        last[i] = gpio_get_level(k_btns[i].pin) != 0;
        // 初始**必须**当作"还没松开"（rel=0），不能直接给 REL。
        //
        // 踩过的坑：给 REL 的话，开机第一次按压无条件被接受。而 GPIO0 是
        // ESP32-S3 的启动 strapping 脚，插拔 USB / 开串口时自动复位电路会
        // 把它拉低一下 —— 实测开机 3.5s 就误触发了一次"BOOT 长按调亮度"。
        // 现在的规则是：必须先看到引脚稳定高电平 REL 次（= 确认松开了），
        // 之后的高→低才算一次有效按压。误触发就被挡在外面了。
        rel[i]  = 0;
    }

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // 每轮取一次即可：状态由 wifi_mgr 的事件回调更新，轮询频率 50Hz 够跟
        bool wifi_ok = (wifi_mgr_state() == WIFI_ST_CONNECTED);

        for (size_t i = 0; i < BTN_NUM; i++) {
            bool now = gpio_get_level(k_btns[i].pin) != 0;

            if (now == last[i]) {
                cnt[i] = 0;
                if (now && rel[i] < REL) rel[i]++;   // 持续高电平 → 累计松开时间

                // 按住不放且已经够久 → 立刻按长按处理一次。
                // 不等松手，否则按 3 秒才调一次亮度，手感很差。
                if (!now && !fired[i] && down_at[i]) {
                    uint32_t held = now_ms - down_at[i];
                    bool is_boot = (k_btns[i].pin == BOARD_BTN_BOOT);

                    if (is_boot && !wifi_ok) {
                        // **只有没联网时**，BOOT 长按才让给配网。
                        //
                        // 已联网时 BOOT 保持原语义（700ms 调亮度），按住多久都
                        // 不会进配网 —— 配网会中断数据轮询，联网状态下不应该
                        // 被误触。要换网络就先断网（或重新配网后重启）。
                        //
                        // 未联网时 700ms 什么都不做：那个状态下用户按 3 秒一定
                        // 是想配网，先跳一档亮度只会让人困惑。
                        if (held >= PROV_MS) {
                            fired[i] = true;
                            ESP_LOGW(TAG, ">>> BOOT 长按 %lums（未联网），进入配网",
                                     (unsigned long)held);
                            s_prov_req = true;      // 由 LVGL 任务执行
                        }
                    } else if (is_boot && wifi_mgr_svr_down()) {
                        /* WiFi 是通的、但服务连不上：长按 BOOT 3 秒重新开放
                         * 配置页。正常情况页面在连不上时已经自己开了，
                         * 这里是给"手滑关掉页面 / 想再看一眼地址"留的入口。
                         * 同样不触发 700ms 的亮度循环，理由同上。 */
                        if (held >= PROV_MS) {
                            fired[i] = true;
                            ESP_LOGW(TAG, ">>> BOOT 长按 %lums（服务连不上），重开配置页",
                                     (unsigned long)held);
                            s_svrcfg_req = true;
                        }
                    } else if (held >= LONG_MS) {
                        fired[i] = true;
                        // 调亮度前先唤醒：休眠屏是 30% 背光，
                        // 在这个状态下按"调亮"却看不到屏幕变亮，
                        // 用户只会以为按键没反应。唤醒后亮度回到用户档位，
                        // 后续的 ☀ 提示也才有意义。
                        s_wake_req = true;
                        int step = (k_btns[i].pin == BOARD_BTN_VOL_UP)   ? +1
                                 : (k_btns[i].pin == BOARD_BTN_VOL_DOWN) ? -1
                                 : 0;
                        button_action(k_btns[i].name, step);
                    }
                }
                continue;
            }
            if (++cnt[i] < NEED) continue;

            cnt[i]  = 0;
            last[i] = now;

            if (!now) {                              // 高→低 = 按下
                if (rel[i] >= REL) {                 // 之前确实松开了才算一次有效按压
                    down_at[i] = now_ms;
                    fired[i]   = false;
                }
                rel[i] = 0;
            } else {                                 // 低→高 = 松开
                // 短按（没走长按分支）→ 下一屏。
                // **不能直接调 page_apply**：它要动控件，而 LVGL 不是线程安全的，
                // 这里只投递请求，由 lvgl_task 在持有 LVGL 上下文时执行。
                if (down_at[i] && !fired[i]) {
                    s_page_req = s_page + 1;
                    // 不管是不是在休眠，都投一个唤醒请求：
                    // 睡着时按键 → 亮屏；醒着时 → sleep_apply(false) 是空操作
                    // （内部有 on == s_asleep 的判等），但保活时间会被续上，
                    // 这样用户连按几下翻屏的期间不会中途黑掉。
                    s_wake_req = true;
                }
                down_at[i] = 0;
                fired[i]   = false;
                rel[i]     = 0;                      // 刚松开，重新计时
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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
// 失败计数说了算（连续 5 次），这里只负责把地址显示出来。
//
// 参与比对的还有"地址版本号" —— 用户改了地址之后要立刻把新地址显示出来，
// 不然屏上还挂着旧 IP，会让人以为没改成功。
static void svr_ui_sync(void)
{
    static bool     last_down = false;
    static uint32_t last_ver  = 0xFFFFFFFF;
    static char     last_ip[16] = "";
    static char     last_tok[8] = "";

    // 只有 WiFi 通着时"连不上服务"才有意义：WiFi 都断了的话那是 WiFi 浮层的事
    bool       down = wifi_mgr_svr_down() && (wifi_mgr_state() == WIFI_ST_CONNECTED);
    uint32_t   ver  = wifi_mgr_svr_version();
    const char *ip  = wifi_mgr_ip();
    /* 验证码 30 秒换一次，所以它也要参与比对 —— 不然新码上了屏，
     * 用户还照着旧码填，提交被拒却不知道为什么。 */
    const char *tok = down ? wifi_mgr_cfg_token() : "";

    // IP 也参与比对：路由器重新分了 IP 的话，屏上那个配置页地址得跟着变
    if (down == last_down && ver == last_ver && strcmp(ip, last_ip) == 0
        && strcmp(tok, last_tok) == 0) return;
    last_down = down;
    last_ver  = ver;
    strncpy(last_ip, ip, sizeof(last_ip) - 1);
    strncpy(last_tok, tok, sizeof(last_tok) - 1);

    if (!down) {
        lv_obj_add_flag(s_pg_svr, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_pg_svr, LV_OBJ_FLAG_HIDDEN);

    // 省掉 http:// 前缀：240px 屏上一行要塞下 IP 和端口，前缀没信息量
    const char *b = wifi_mgr_svr_base();
    if (strncmp(b, "http://", 7) == 0) b += 7;
    lv_label_set_text(s_lbl_svr_l1, b[0] ? b : "未配置");

    // 第二行是用户真正要用的东西：设备自己在局域网里的 IP = 配置页地址
    lv_label_set_text(s_lbl_svr_l2, ip[0] ? ip : "--");

    // 第三行提示 + 验证码。配置页无鉴权（跑在真实局域网上），
    // 改服务地址必须带这个码 —— 屏幕是唯一的分发渠道。
    if (tok[0]) {
        lv_label_set_text(s_lbl_svr_l3, "用浏览器打开上面地址\n照着下面的数字填验证码");
        char buf[32];
        snprintf(buf, sizeof(buf), "验证码 %s", tok);
        lv_label_set_text(s_lbl_svr_tok, buf);
    } else {
        lv_label_set_text(s_lbl_svr_l3, "用浏览器打开上面地址\n进去改服务地址");
        lv_label_set_text(s_lbl_svr_tok, "");
    }
}

// ===========================================================================
// LVGL 任务
// ===========================================================================

static void lvgl_task(void *arg)
{
    uint32_t last_toast = 0xFFFFFFFF;

    while (1) {
        // 按键产生的亮度变更，统一在 LVGL 任务里落实
        if (s_bright_apply) {
            s_bright_apply = 0;
            backlight_set(k_bright_steps[s_bright_idx]);
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

        // 按键唤醒：休眠中按任意键，先把人叫起来再干活。
        //
        // 顺序有讲究 —— 必须在 page_apply 之后执行：
        // 用户可能是"想看下一屏"而按的键，page_apply 已经把人送到新屏了，
        // 这里只需要把屏幕点亮。反过来先唤醒再切屏会出现一帧的跳动。
        //
        // 保活 60 秒：这段时间里即使仍然空闲也不睡回去（理由见 s_awake_until_ms）。
        if (s_wake_req) {
            s_wake_req       = false;
            s_awake_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + SLEEP_GRACE_MS;
            sleep_apply(false);
        }

        // 配网请求：按键任务只置标志，真正开热点/起 HTTP 服务在这里做。
        // 那两个动作会阻塞几百毫秒，放按键任务里会让长按判定失灵。
        if (s_prov_req) {
            s_prov_req = false;
            wifi_mgr_start_provision();
        }

        // 重开服务地址配置页
        if (s_svrcfg_req) {
            s_svrcfg_req = false;
            wifi_mgr_start_svr_config();
        }

        // WiFi 状态变了就更新浮层（内部有状态去重，每帧调用开销可忽略）
        wifi_ui_sync();
        svr_ui_sync();

        // 按键提示：显示 1.5 秒后自动消失
        if (s_toast_val >= 0) {
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

        // ⚠ 只在锁内拷快照，绘制放锁外。
        // 原来这里是"持锁调 ui_render()"，LVGL 绘制全在锁里做完。
        // 绘制要走 SPI、可能几十毫秒，期间 fetch 任务拿不到锁就超时，
        // 那一轮拉到的数据被静默丢弃（还看不出报错，只是屏幕不更新）。
        // 锁内只做一次 memcpy（board_data_t 是纯 POD，几百字节，微秒级），
        // 拿到快照后立刻放锁，再慢慢画。
        board_data_t snap;
        if (xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            snap = s_data;
            xSemaphoreGive(s_data_lock);
            ui_render(&snap);
        }
        uint32_t wait = lv_timer_handler();
        if (wait < 5)  wait = 5;
        if (wait > 50) wait = 50;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

// ===========================================================================
// 入口
// ===========================================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== WorkBuddy 看板启动 ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_data_lock = xSemaphoreCreateMutex();

    backlight_init();
    backlight_set(0);

    // lv_init() 必须在 display_init() 之前！
    // display_init() 内部要调 lv_display_create / lv_display_set_buffers，
    // 顺序反了 LVGL 内部结构还没初始化就往堆里写，会直接踩坏堆：
    //   assert failed: remove_free_block tlsf_control_functions.h:372
    lv_init();
    lv_tick_set_cb([](void) -> uint32_t {
        return (uint32_t)(esp_timer_get_time() / 1000);
    });

    display_init();

    font_probe();

    // 电量采样与 WiFi 无关，越早开越好 —— 首帧就能带出真实百分比，
    // 而不是先显示 "--" 再跳变。放在 ui_build() 之前即可。
    battery_init();

    ui_build();

    // 先渲染一帧再开背光，避免上电白闪
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(300));
    backlight_set(k_bright_steps[s_bright_idx]);   // 默认 100%

    button_init();

    // 提示音。放在 button_init 之后、wifi 之前：
    // I2S 初始化只依赖 GPIO，与网络无关；早开的好处是第一次
    // "任务完成"事件就能响，不会因为还在连 WiFi 而静音。
    // 失败不影响主流程（内部只打日志、不 abort）。
    audio_init();

    // 温度传感器：装不上不影响看板，只是页脚显示 --
    tsens_init();

    /* 编译期 BRIDGE_BASE 只当"一次都没配过"时的初值。
     * NVS 里配过就以 NVS 为准 —— 电脑换 IP 在配置页改一下就行，不用重烧。 */
    wifi_mgr_set_default_base(BRIDGE_BASE);

    wifi_mgr_begin();
#ifdef CONFIG_WB_LOCAL_TIME
    time_init();
#endif

    /* ⚠ 检查 xTaskCreate 返回值。
     * 失败时任务根本没起来，但设备看起来是正常的（能亮屏、能连 WiFi），
     * 只是数据永远不刷新 —— 静默失效，最难排查。 */
    if (xTaskCreate(fetch_task,  "fetch",  8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "fetch 任务创建失败 —— 屏幕不会更新数据");
    }
    if (xTaskCreate(lvgl_task,   "lvgl",   8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "lvgl 任务创建失败 —— 屏幕不会刷新");
    }
    if (xTaskCreate(button_task, "button", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "button 任务创建失败 —— 按键无效");
    }
}
