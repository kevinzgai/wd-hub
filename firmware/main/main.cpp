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
        ESP_LOGW(TAG, "FONT %-9s U+%04X found=%d placeholder=%d box=%dx%d ofs=(%d,%d) adv=%u",
                 list[i].name, (unsigned)list[i].cp, (int)ok, (int)d.is_placeholder,
                 (int)d.box_w, (int)d.box_h, (int)d.ofs_x, (int)d.ofs_y,
                 (unsigned)d.adv_w);
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

#ifndef WIFI_SSID
#define WIFI_SSID         "SCB"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD     "12345677"
#endif
// 本机以太网 192.168.0.89，设备必须连同一个路由器
// 只给 base：第 0 屏走 /status，其余屏走 /screen/<i>，路径由 s_page 决定
// 端口 27777 与服务端 DEFAULT_PORT 保持一致（刻意避开 8080）
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

typedef struct {
    char    t[TEXT_ROW];
    char    v[TEXT_ROWVAL];
    uint8_t color;             // 0 普通 / 1 强调蓝 / 2 成功绿 / 3 警告黄 / 4 暗灰
} row_t;

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
    int       working;
    int       today_done;
    int       automations;
    int       cpu;          // %，-1 = 服务端没给该字段（旧版服务）
    int       mem;          // %
    int       disk;         // %
    int       temp;         // °C，0 = 取不到（Windows 上通常如此）
    session_t sessions[MAX_SESSIONS];
    int       session_count;
    // ---- 详情屏 ----
    char      page_title[24];
    row_t     rows[MAX_ROWS];
    int       row_count;
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
// 外圈弧 = 进度，中心文字 = 百分比。充电时弧变绿并绕行。
static lv_obj_t *s_arc_bat;        // 进度弧
static lv_obj_t *s_lbl_bat;        // 中心百分比文字
static lv_obj_t *s_lbl_chrg;       // 充电时在圆圈下方补一个闪电

// ---- 多屏 ----
// 两组控件放在各自的容器里，切页只是切 HIDDEN，不销毁重建。
// 页头（logo/时钟/日期/状态）和页脚两组屏共用，所以留在 scr 上。
static lv_obj_t *s_pg_main;        // 第 0 屏：三格统计 + 会话列表
static lv_obj_t *s_pg_list;        // 第 1..N-1 屏：标题 + 6 行
static lv_obj_t *s_lbl_page;       // 详情屏标题
static lv_obj_t *s_lbl_row_t[MAX_ROWS];
static lv_obj_t *s_lbl_row_v[MAX_ROWS];

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

// 按键写入的配网请求，由 LVGL 任务消费（wifi_mgr_start_provision 里有
// esp_wifi_set_mode / httpd_start，放按键任务里跑会长时间阻塞按键扫描）
static volatile bool s_prov_req = false;

// 标题只在内容变化时才 set_text —— 每次重设都会把跑马灯位置打回开头，
// 服务端 5 秒推一轮，不判等的话滚动会一直"闪回"。
static char     s_title_cache[MAX_SESSIONS][48];
static char     s_meta_cache[MAX_SESSIONS][64];
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
//   y=0    09:44        10-26  ◕     28px 时钟行（右侧圆圈=电量）
//   y=34   ─────────────────────     1px 分隔
//   y=40   [ 1 ] [ 1 ] [ 5 ]        46px 三格统计
//   y=92   ● 会话标题 1              40px
//          project · 5m
//   y=132  ● 会话标题 2
//   y=172  ● 会话标题 3
//   y=216  ─────────────────────
//   y=220  更新 09:44:12            20px 页脚
// ===========================================================================

// 电量圆圈几何。34px 直径塞进 0..36 的页头行（y=1..35，留 1px 呼吸）。
// 减掉弧宽 3px 后内径 26px，font_cn_12 的 ASCII 是 6px/字符，
// 三字符 "100%" 正好 24px，放得下，不用为 100% 做特殊处理。
#define BAT_ARC_SIZE    34
#define BAT_ARC_WIDTH   3
#define BAT_ARC_X       (-6)            // 距右边 6px
#define BAT_ARC_Y       1
// 日期/状态右对齐的偏移 = 6(右边距) + 34(圆圈) + 6(间隔)
#define BAT_TEXT_RIGHT  (-46)

// 页头整行下移量。原来 logo 顶边贴 y=0，时钟（30px 行高）以 logo 中心对齐后
// 顶边落到 -3，被屏幕顶裁掉；下移 4px 后 logo 占 4..28、时钟 1..31，都完整可见。
#define HEADER_LOGO_Y   4

// 电量数字的视觉居中补偿。
// LVGL 绘制字形用的是（见 lv_draw_label.c:671）：
//   glyph_y1 = pos->y + (line_height - base_line) - box_h - ofs_y
// font_cn_12: line_height=13, base_line=2；数字 box_h=10, ofs_y=0
//   → glyph_y1 = 0 + (13-2) - 10 - 0 = 1，字高 10，底边在 11
// 行框 13px 内：上方空 1px、下方空 2px → 墨迹重心比行框中心高 0.5px。
// lv_obj_align_to(CENTER) 对齐的是行框，所以要往下补 1px 才是视觉正中。
#define BAT_LABEL_DY    1
// 横向同理：数字字形有 ofs_x=1（左边留 1px 空白），"100%" 总宽 24px 但墨迹
// 实际只占 x=1..22，视觉重心偏右 0.5px。
// 先按 -5 左移，用户反馈"多了"，再右移 2px -> -3。
#define BAT_LABEL_DX    (-3)

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
    lv_obj_set_style_arc_rounded(s_arc_bat, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc_bat, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_arc_bat, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_arc_bat, 0, LV_PART_MAIN);

    // 中心百分比。font_cn_12 的 ASCII 是 6px/字符，34px 圆圈内径 26px，
    // "100%" 占 24px 放得下 —— 全部带百分号，口径统一。
    // 用 align_to(CENTER) 之后再加 BAT_LABEL_DY：CENTER 对齐的是行框(13px)，
    // 而数字墨迹只有 10px 且下方留白比上方多 1px，直接居中会显得偏高。
    s_lbl_bat = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_bat, &font_cn_12, 0);
    lv_obj_set_style_text_color(s_lbl_bat, COL_TEXT, 0);
    lv_label_set_text(s_lbl_bat, "--");
    lv_obj_align_to(s_lbl_bat, s_arc_bat, LV_ALIGN_CENTER, BAT_LABEL_DX, BAT_LABEL_DY);

    // 充电闪电：字体里没有 ⚡，用 ASCII 字符拼不行，
    // 改为在圆圈右上角点一个小绿点表示正在充电。
    s_lbl_chrg = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_chrg, &font_cn_12, 0);
    lv_obj_set_style_text_color(s_lbl_chrg, COL_OK, 0);
    lv_label_set_text(s_lbl_chrg, "");
    lv_obj_align_to(s_lbl_chrg, s_arc_bat, LV_ALIGN_TOP_RIGHT, 2, -3);

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
}

// ===========================================================================
// 渲染（调用方需持 s_data_lock）
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
static void page_apply(int page)
{
    if (s_nscr < 1) s_nscr = 1;
    page = ((page % s_nscr) + s_nscr) % s_nscr;   // 环形，负数也安全
    if (page == s_page) return;

    s_page = page;

    if (page == 0) {
        lv_obj_clear_flag(s_pg_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        ui_list_reset();
        lv_obj_add_flag(s_pg_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pg_list, LV_OBJ_FLAG_HIDDEN);
    }

    // 叫醒拉取任务立刻换 URL 重拉，不等下一个 5 秒周期
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
    ESP_LOGI(TAG, ">>> 切屏 -> %d/%d", page, s_nscr);
}

// 电量圆圈渲染。只在 LVGL 任务里调用（和 ui_render 同线程）。
//
// 分档着色：
//   >50%  绿   20~50%  黄   <20%  红
// 充电中：进度弧固定绿色，并在圆圈右上角显示 "+" 标记。
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
    static int  last_pct  = -2;
    static bool last_chrg = false;

    if (!b.valid) {
        if (last_pct != -1) {
            last_pct = -1;
            lv_arc_set_value(s_arc_bat, 0);
            lv_label_set_text(s_lbl_bat, "--");
            lv_obj_set_style_text_color(s_lbl_bat, COL_DIM, 0);
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
        lv_color_t col;
        if (b.charging)     col = COL_OK;
        else if (pct > 50)  col = COL_OK;
        else if (pct >= 20) col = COL_WARN;
        else                col = COL_ERR;

        lv_obj_set_style_arc_color(s_arc_bat, col, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_lbl_bat,
                                    (pct < 20 && !b.charging) ? col : COL_TEXT, 0);

        lv_label_set_text_fmt(s_lbl_bat, "%d%%", pct);

        // 充电标记。字体没有 ⚡，用 "+"。
        // 放电时不显示，避免和百分比抢视觉。
        lv_label_set_text(s_lbl_chrg, b.charging ? "+" : "");
    }
}

static void ui_render(const board_data_t *d)
{
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

    // ---- 页脚（两屏共用）----
    // 左：本机 CPU / 内存 / 磁盘占用。超阈值时整条变黄，
    // 12px 字号做不到逐段着色，整条变色足够提醒。
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

    char footer[24];
    snprintf(footer, sizeof(footer), "★ %s", d->clock);
    lv_label_set_text(s_lbl_footer, footer);

    // 宠物表情跟着数据走（本屏两页共用，放在这里只算一次）
    pet_update_state(d);

    // ---- 主体 ----
    // 回包可能属于"切页前"发出的那个请求（d->page 落后于 s_page），
    // 这时只更新页头页脚，主体等下一轮，别把上一屏的字画到新屏上
    if (d->page != s_page) return;

    if (s_page == 0) ui_render_overview(d);
    else             ui_render_list(d);
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

    while (*p) {
        if (*p == '{') {
            if (depth == 0) {
                idx++;
                if (idx == n) obj_start = p;
            }
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0 && obj_start) {
                *start = obj_start;
                *end   = p;
                return true;
            }
        } else if (*p == ']' && depth == 0) {
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
    d->working     = json_int(json, "working", 0);
    d->today_done  = json_int(json, "today_done", 0);
    d->automations = json_int(json, "automations", 0);
    // 缺省 -1 表示"服务端没下发"，页脚显示 --，不与真实的 0% 混为一谈
    d->cpu  = json_int(json, "cpu",  -1);
    d->mem  = json_int(json, "mem",  -1);
    d->disk = json_int(json, "disk", -1);
    d->temp = json_int(json, "temp",  0);
    // 屏数：/status 和 /screen 都下发 nscr，固件据此决定按几屏循环
    d->nscr = json_int(json, "nscr", 0);

    // ---- 详情屏：页标题 + rows 数组 ----
    json_str(json, "t", d->page_title, sizeof(d->page_title));
    for (int i = 0; i < MAX_ROWS; i++) {
        const char *start = NULL, *end = NULL;
        if (!json_nth_object(json, "rows", i, &start, &end)) break;

        char sub[256];
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

static void time_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_init();
}

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
                    } else if (held >= LONG_MS) {
                        fired[i] = true;
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

        // 配网请求：按键任务只置标志，真正开热点/起 HTTP 服务在这里做。
        // 那两个动作会阻塞几百毫秒，放按键任务里会让长按判定失灵。
        if (s_prov_req) {
            s_prov_req = false;
            wifi_mgr_start_provision();
        }

        // WiFi 状态变了就更新浮层（内部有状态去重，每帧调用开销可忽略）
        wifi_ui_sync();

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

    /* 编译期 BRIDGE_BASE 只当"一次都没配过"时的初值。
     * NVS 里配过就以 NVS 为准 —— 电脑换 IP 在配置页改一下就行，不用重烧。 */
    wifi_mgr_set_default_base(BRIDGE_BASE);

    wifi_mgr_begin();
    time_init();

    xTaskCreate(fetch_task,  "fetch",  8192, NULL, 5, NULL);
    xTaskCreate(lvgl_task,   "lvgl",   8192, NULL, 4, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
}
