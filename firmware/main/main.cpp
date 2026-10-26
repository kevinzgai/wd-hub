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

#include "lvgl.h"
#include "board_config.h"

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
#ifndef BRIDGE_URL
#define BRIDGE_URL        "http://192.168.0.89:8080/status"
#endif

#define POLL_INTERVAL_MS  (5 * 1000)

// ===========================================================================
// 数据模型
// ===========================================================================

#define MAX_SESSIONS 3
#define TEXT_TITLE   40
#define TEXT_PROJECT 20
#define TEXT_ELAPSED 12

typedef struct {
    char title[TEXT_TITLE];
    char project[TEXT_PROJECT];
    char elapsed[TEXT_ELAPSED];
} session_t;

typedef struct {
    bool      ok;
    char      error[64];
    char      clock[8];
    char      date[8];
    int       working;
    int       today_done;
    int       automations;
    session_t sessions[MAX_SESSIONS];
    int       session_count;
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
static lv_obj_t *s_lbl_footer;
static lv_obj_t *s_lbl_toast;      // 按键操作的临时提示

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

// 深色底 + 高对比，小屏远看更清楚
#define COL_BG      lv_color_hex(0x0B0E14)
#define COL_TEXT    lv_color_hex(0xE6EDF3)
#define COL_DIM     lv_color_hex(0x7D8590)
#define COL_ACCENT  lv_color_hex(0x58A6FF)
#define COL_OK      lv_color_hex(0x3FB950)
#define COL_WARN    lv_color_hex(0xD29922)
#define COL_ERR     lv_color_hex(0xF85149)

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
//   y=0    09:44          10-26      28px 时钟行
//   y=34   ─────────────────────     1px 分隔
//   y=40   [ 1 ] [ 1 ] [ 5 ]        46px 三格统计
//   y=92   ● 会话标题 1              40px
//          project · 5m
//   y=132  ● 会话标题 2
//   y=172  ● 会话标题 3
//   y=216  ─────────────────────
//   y=220  更新 09:44:12            20px 页脚
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

static void mk_hline(lv_obj_t *parent, lv_coord_t y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, 224, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(line, COL_DIM, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_30, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

static void ui_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 时钟 ----
    s_lbl_clock = mk_label(scr, &font_cn_28, COL_TEXT, LV_ALIGN_TOP_LEFT, 8, -4);
    lv_label_set_text(s_lbl_clock, "--:--");

    s_lbl_date = mk_label(scr, &font_cn_12, COL_DIM, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_label_set_text(s_lbl_date, "--");

    s_lbl_status = mk_label(scr, &font_cn_12, COL_DIM, LV_ALIGN_TOP_RIGHT, -8, 20);
    lv_label_set_text(s_lbl_status, "● 连接中");

    // ---- 符号渲染测试条（临时，验证完可删）----
    // 放在时钟和日期之间的空白区，16px 下看 ★⚡✓☀ 的清晰度
    lv_obj_t *sym_test = mk_label(scr, &font_cn_16, COL_WARN,
                                  LV_ALIGN_TOP_LEFT, 92, 6);
    lv_label_set_text(sym_test, "★⚡✓☀");

    mk_hline(scr, 36);

    // ---- 三格统计（每格 72 宽 + 4 间距，共 224）----
    static const char *captions[3] = { "⚙ 工作", "✓ 完成", "⚡ 自动" };
    static const lv_color_t colors[3] = { COL_ACCENT, COL_OK, COL_WARN };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *box = lv_obj_create(scr);
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
    for (int i = 0; i < MAX_SESSIONS; i++) {
        lv_coord_t y = 94 + i * 40;

        s_lbl_sess_title[i] = mk_label(scr, &font_cn_16, COL_TEXT,
                                       LV_ALIGN_TOP_LEFT, 8, y);
        lv_label_set_text(s_lbl_sess_title[i], "");
        lv_obj_set_width(s_lbl_sess_title[i], 224);
        lv_label_set_long_mode(s_lbl_sess_title[i], LV_LABEL_LONG_DOT);

        s_lbl_sess_meta[i] = mk_label(scr, &font_cn_12, COL_DIM,
                                      LV_ALIGN_TOP_LEFT, 8, y + 19);
        lv_label_set_text(s_lbl_sess_meta[i], "");
        lv_obj_set_width(s_lbl_sess_meta[i], 224);
        lv_label_set_long_mode(s_lbl_sess_meta[i], LV_LABEL_LONG_DOT);

        lv_obj_add_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ---- 页脚 ----
    mk_hline(scr, 216);
    s_lbl_footer = mk_label(scr, &font_cn_12, COL_DIM, LV_ALIGN_TOP_LEFT, 8, 222);
    lv_label_set_text(s_lbl_footer, "等待数据");

    // ---- 按键提示（默认隐藏，只在按的时候闪一下）----
    s_lbl_toast = mk_label(scr, &font_cn_16, COL_ACCENT,
                           LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_add_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);
}

// ===========================================================================
// 渲染（调用方需持 s_data_lock）
// ===========================================================================

static void ui_render(void)
{
    if (!s_data.ok) {
        lv_obj_set_style_text_color(s_lbl_status, COL_ERR, 0);
        lv_label_set_text(s_lbl_status, "● 离线");
        char fbuf[96], ebuf[64];
        utf8_sanitize(&font_cn_12,
                      s_data.error[0] ? s_data.error : "无法连接 bridge",
                      ebuf, sizeof(ebuf));
        snprintf(fbuf, sizeof(fbuf), "✗ %s", ebuf);
        lv_label_set_text(s_lbl_footer, fbuf);
        return;   // 保留上次数据，避免清零闪屏
    }

    lv_obj_set_style_text_color(s_lbl_status, COL_OK, 0);
    lv_label_set_text(s_lbl_status, "● 在线");

    lv_label_set_text(s_lbl_clock, s_data.clock);
    lv_label_set_text(s_lbl_date, s_data.date);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_data.working);
    lv_label_set_text(s_lbl_stat[0], buf);
    snprintf(buf, sizeof(buf), "%d", s_data.today_done);
    lv_label_set_text(s_lbl_stat[1], buf);
    snprintf(buf, sizeof(buf), "%d", s_data.automations);
    lv_label_set_text(s_lbl_stat[2], buf);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (i < s_data.session_count) {
            char title[48], proj[24];
            utf8_sanitize(&font_cn_12, s_data.sessions[i].title,   title, sizeof(title));
            utf8_sanitize(&font_cn_12, s_data.sessions[i].project, proj,  sizeof(proj));
            lv_label_set_text(s_lbl_sess_title[i], title);

            char meta[64];
            snprintf(meta, sizeof(meta), "%s · %s",
                     proj, s_data.sessions[i].elapsed);
            lv_label_set_text(s_lbl_sess_meta[i], meta);

            lv_obj_clear_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_sess_title[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_sess_meta[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_data.session_count == 0) {
        lv_label_set_text(s_lbl_sess_title[0], "空闲中");
        lv_obj_set_style_text_color(s_lbl_sess_title[0], COL_DIM, 0);
        lv_obj_clear_flag(s_lbl_sess_title[0], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_color(s_lbl_sess_title[0], COL_TEXT, 0);
    }

    char footer[32];
    snprintf(footer, sizeof(footer), "★ 更新 %s", s_data.clock);
    lv_label_set_text(s_lbl_footer, footer);
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
        d->session_count++;
    }
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
    accum_t acc = { s_http_buf, 0, HTTP_BUF_SIZE };

    esp_http_client_config_t cfg = {};
    cfg.url        = BRIDGE_URL;
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

    while (1) {
        acc.len = 0;
        s_http_buf[0] = '\0';

        esp_err_t err = esp_http_client_perform(client);
        int status    = esp_http_client_get_status_code(client);

        board_data_t parsed = {};
        bool got = (err == ESP_OK && status == 200 && acc.len > 0);

        if (got) {
            parse_into(&parsed, s_http_buf);
            fails = 0;
            ESP_LOGI(TAG, "OK working=%d done=%d auto=%d sess=%d btn=%s",
                     parsed.working, parsed.today_done,
                     parsed.automations, parsed.session_count,
                     (const char *)s_last_btn);
        } else {
            fails++;
            ESP_LOGW(TAG, "拉取失败 err=%s status=%d (连续%d次)",
                     esp_err_to_name(err), status, fails);
            parsed.ok = false;
            snprintf(parsed.error, sizeof(parsed.error), "HTTP %d", status);

            if (xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
                // 短暂失败沿用旧数据，只把状态点变红
                bool had = s_data.ok;
                if (had) parsed = s_data;
                parsed.ok = false;
                xSemaphoreGive(s_data_lock);
            }
        }

        if (xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_data = parsed;
            xSemaphoreGive(s_data_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(fails > 3 ? 15000 : POLL_INTERVAL_MS));
    }
}

// ===========================================================================
// WiFi
// ===========================================================================

static EventGroupHandle_t s_wifi_ev;

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 断开，2s 后重连");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void wifi_init(void)
{
    s_wifi_ev = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_handler, NULL, NULL));

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "连接 WiFi: %s", WIFI_SSID);
}

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

    bool last[BTN_NUM];
    int  cnt[BTN_NUM]  = {0};
    int  rel[BTN_NUM];

    for (size_t i = 0; i < BTN_NUM; i++) {
        last[i] = gpio_get_level(k_btns[i].pin) != 0;
        rel[i]  = REL;    // 开机就当作已松开
    }

    while (1) {
        for (size_t i = 0; i < BTN_NUM; i++) {
            bool now = gpio_get_level(k_btns[i].pin) != 0;

            if (now == last[i]) {
                cnt[i] = 0;
                if (now && rel[i] < REL) rel[i]++;   // 持续高电平 → 累计松开时间
                continue;
            }
            if (++cnt[i] < NEED) continue;

            cnt[i]  = 0;
            last[i] = now;

            if (!now) {                              // 高→低 = 按下
                if (rel[i] >= REL) {                 // 之前确实松开了才算一次有效按压
                    int step = (k_btns[i].pin == BOARD_BTN_VOL_UP)   ? +1
                             : (k_btns[i].pin == BOARD_BTN_VOL_DOWN) ? -1
                             : 0;
                    button_action(k_btns[i].name, step);
                }
                rel[i] = 0;
            } else {
                rel[i] = 0;                          // 刚松开，重新计时
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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

        if (xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            ui_render();
            xSemaphoreGive(s_data_lock);
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

    ui_build();

    // 先渲染一帧再开背光，避免上电白闪
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(300));
    backlight_set(k_bright_steps[s_bright_idx]);   // 默认 100%

    button_init();

    wifi_init();
    time_init();

    xTaskCreate(fetch_task,  "fetch",  8192, NULL, 5, NULL);
    xTaskCreate(lvgl_task,   "lvgl",   8192, NULL, 4, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
}
