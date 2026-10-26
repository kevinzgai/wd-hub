// 最小屏幕验证 demo · 星智 1.54TFT WiFi (ESP32-S3)
//
// 目标：验证硬件与工具链，不引入 WiFi / LVGL / 组件依赖。
// 只做一件事：驱动 ST7789 刷出可辨识的画面，然后循环变色。
//
// 引脚来源：78/xiaozhi-esp32 main/boards/nologo/xingzhi-cube-1.54tft-wifi/config.h

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"

static const char *TAG = "demo";

// ---- 引脚 ----
#define LCD_MOSI  10
#define LCD_SCLK  9
#define LCD_DC    8
#define LCD_CS    14
#define LCD_RST   18
#define LCD_BL    13

#define H_RES 240
#define V_RES 240
// 每次刷 20 行，缓冲 240*20*2 = 9.6KB，内部 DMA RAM 绰绰有余
#define CHUNK_LINES 20

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_line_buf;
static int s_hue_shift = 0;

static void backlight_init(void)
{
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num       = LEDC_TIMER_0;
    t.freq_hz         = 5000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {};
    c.gpio_num   = LCD_BL;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_0;
    c.timer_sel  = LEDC_TIMER_0;
    c.duty       = 0;
    c.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void backlight_set(uint32_t percent)
{
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void lcd_init(void)
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = LCD_SCLK;
    bus.mosi_io_num     = LCD_MOSI;
    bus.miso_io_num     = -1;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = H_RES * CHUNK_LINES * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = LCD_CS;
    io_cfg.dc_gpio_num       = LCD_DC;
    io_cfg.spi_mode          = 3;     // 与官方 board config 一致
    io_cfg.pclk_hz           = 40 * 1000 * 1000;   // 先用 40MHz 保稳
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                             &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = LCD_RST;
    pcfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    pcfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    // 这块屏必须反色（官方 board 代码同为 true）
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// 把整个屏幕刷成纯色
static void fill_screen(uint16_t color)
{
    for (int i = 0; i < H_RES * CHUNK_LINES; i++) {
        s_line_buf[i] = color;
    }
    for (int y = 0; y < V_RES; y += CHUNK_LINES) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, H_RES, y + CHUNK_LINES, s_line_buf);
    }
}

// 三色竖条，用来判断是否花屏 / 颜色通道是否正确
static void draw_bars(void)
{
    static const uint16_t colors[3] = {
        0xF800,   // 红
        0x07E0,   // 绿
        0x001F,   // 蓝
    };
    for (int y = 0; y < V_RES; y += CHUNK_LINES) {
        for (int line = 0; line < CHUNK_LINES; line++) {
            for (int x = 0; x < H_RES; x++) {
                int band = x * 3 / H_RES;
                s_line_buf[line * H_RES + x] = colors[band];
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, H_RES, y + CHUNK_LINES, s_line_buf);
    }
}

// 渐变，验证刷新率与无撕裂
static void draw_gradient(int shift)
{
    for (int y = 0; y < V_RES; y += CHUNK_LINES) {
        for (int line = 0; line < CHUNK_LINES; line++) {
            int yy = y + line;
            for (int x = 0; x < H_RES; x++) {
                uint8_t r = (uint8_t)((x + shift) & 0xFF);
                uint8_t g = (uint8_t)((yy + shift) & 0xFF);
                uint8_t b = (uint8_t)((x + yy + shift) & 0xFF);
                s_line_buf[line * H_RES + x] = rgb565(r, g, b);
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, H_RES, y + CHUNK_LINES, s_line_buf);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 屏幕验证 demo 启动 ===");

    backlight_init();
    backlight_set(0);          // 先黑屏，避免上电白闪

    lcd_init();
    ESP_LOGI(TAG, "ST7789 初始化完成 %dx%d", H_RES, V_RES);

    s_line_buf = heap_caps_malloc(H_RES * CHUNK_LINES * sizeof(uint16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_line_buf) {
        ESP_LOGE(TAG, "行缓冲分配失败");
        abort();
    }

    backlight_set(70);
    ESP_LOGI(TAG, "背光已开，开始刷屏");

    while (1) {
        // 1. 纯色轮播 —— 能辨认屏幕是否工作
        ESP_LOGI(TAG, "纯红");
        fill_screen(0xF800);
        vTaskDelay(pdMS_TO_TICKS(1200));

        ESP_LOGI(TAG, "纯绿");
        fill_screen(0x07E0);
        vTaskDelay(pdMS_TO_TICKS(1200));

        ESP_LOGI(TAG, "纯蓝");
        fill_screen(0x001F);
        vTaskDelay(pdMS_TO_TICKS(1200));

        ESP_LOGI(TAG, "纯白");
        fill_screen(0xFFFF);
        vTaskDelay(pdMS_TO_TICKS(1200));

        // 2. 三色竖条 —— 瑞色通道 / 偏色问题
        ESP_LOGI(TAG, "三色竖条");
        draw_bars();
        vTaskDelay(pdMS_TO_TICKS(2500));

        // 3. 动态渐变 —— 验证刷新性能
        ESP_LOGI(TAG, "渐变动画");
        for (int i = 0; i < 120; i++) {
            draw_gradient(s_hue_shift);
            s_hue_shift = (s_hue_shift + 3) & 0xFF;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}
