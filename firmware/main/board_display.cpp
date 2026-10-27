/* 屏幕驱动 —— 实现（从 main.cpp 搬出，初始化时序与参数一字未改） */
#include "board_display.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"

#include "lvgl.h"

#include "board_config.h"

static const char *TAG = "wb-board";

// ===========================================================================
// LVGL 句柄
// ===========================================================================

static lv_display_t *s_disp;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t    s_panel;

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

void board_display_backlight_set(uint32_t percent)
{
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void board_display_init(void)
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

    // 背光：定时器 + 通道都在这里配好。
    // 开机那一下背光是灭的（duty=0），等 UI 建好再由 main 点亮 ——
    // 避免上电白闪。
    //
    // 注意这个顺序：原来背光初始化排在 display_init 之前（app_main 里
    // backlight_init(); backlight_set(0); 在前）。挪到后面没有可观测差异 ——
    // LEDC 和 SPI 完全独立，且 duty 初始就是 0、配好即灭，
    // 两次调用的净效果都是"背光处于关闭状态"。
    backlight_init();
    board_display_backlight_set(0);
}
