// 星智 1.54TFT WiFi 引脚定义
// 来源：78/xiaozhi-esp32 main/boards/nologo/xingzhi-cube-1.54tft-wifi/config.h
// 芯片已用 esptool flash_id 验证：ESP32-S3 QFN56 rev0.2 + 8MB PSRAM + 16MB Flash
//
// 注意：这里必须用 C 注释 //，不能用 #（那是 Python/Markdown 的写法，
// C 预处理器会把整行当成无效指令直接报错）。引脚要写成 GPIO_NUM_x 枚举，
// 写成裸数字 10 在 C++ 下无法隐式转成 gpio_num_t。

#pragma once

#include "driver/gpio.h"

// ---- 屏幕 ST7789 240x240（SPI3_HOST）----
#define BOARD_LCD_MOSI       GPIO_NUM_10
#define BOARD_LCD_SCLK       GPIO_NUM_9
#define BOARD_LCD_DC         GPIO_NUM_8
#define BOARD_LCD_CS         GPIO_NUM_14
#define BOARD_LCD_RST        GPIO_NUM_18
#define BOARD_LCD_BL         GPIO_NUM_13
#define BOARD_LCD_H_RES      240
#define BOARD_LCD_V_RES      240
#define BOARD_LCD_PIXEL_CLK  (80 * 1000 * 1000)

// ---- 按键 ----
#define BOARD_BTN_BOOT       GPIO_NUM_0
#define BOARD_BTN_VOL_UP     GPIO_NUM_40
#define BOARD_BTN_VOL_DOWN   GPIO_NUM_39

// ---- 音频 I2S（本固件未启用，仅预留）----
// ⚠️ 绝对不要对这些脚调用 gpio_config() / 加内部上下拉。
//    它们是 I2S 信号线，被 codec 偏置在 VDD/2（实测 ~2.0V），
//    一旦被内部拉电阻或被误当 ADC 采样来回翻转，功放会收到
//    无效数据流并发出噪音（本项目踩过这个坑）。
#define BOARD_MIC_WS         GPIO_NUM_4
#define BOARD_MIC_SCK        GPIO_NUM_5
#define BOARD_MIC_DIN        GPIO_NUM_6
#define BOARD_SPK_DOUT       GPIO_NUM_7
#define BOARD_SPK_BCLK       GPIO_NUM_15
#define BOARD_SPK_LRCK       GPIO_NUM_16

// ---- 电池 ----
// 来源：上游 main/boards/nologo/xingzhi-cube-1.54tft-wifi/power_manager.h
//   adc_oneshot_unit_init_cfg_t = { .unit_id = ADC_UNIT_2 }
//   adc_oneshot_config_channel(handle, ADC_CHANNEL_6, ...)
// IDF soc/esp32s3/adc_channel.h: #define ADC2_CHANNEL_6_GPIO_NUM 17
#define BOARD_BAT_GPIO       GPIO_NUM_17
#define BOARD_BAT_ADC_UNIT   2          // ADC_UNIT_2
#define BOARD_BAT_ADC_CH     6          // ADC_CHANNEL_6 -> GPIO17
#define BOARD_CHRG_GPIO      GPIO_NUM_38  // 高电平 = 正在充电
