// 板级引脚定义。同一份固件源码支持两块板，用芯片型号在编译期切分支：
//
//   ESP32-S3  —— 星智 1.54TFT WiFi（240x240，板上只有 BOOT 一颗键）
//   ESP32-C3  —— 上千 AI Passport（240x320，三键 ADC 分压）
//
// C3 参数来源：passport-os 仓库 components/bsp/include/bsp_pins.h
//   https://gitee.com/alex_kan/passport-os  (MIT License, HEAD b4d9555)
//   只照抄引脚/时序常量，不拷贝其代码实现；本工程对应驱动按同一份
//   寄存器/引脚规格独立实现（见 board_display.cpp / buttons_adc.cpp /
//   battery_cw2017.cpp）。
//
// 注意：这里必须用 C 注释 //，不能用 #（那是 Python/Markdown 的写法，
// C 预处理器会把整行当成无效指令直接报错）。引脚要写成 GPIO_NUM_x 枚举，
// 写成裸数字 10 在 C++ 下无法隐式转成 gpio_num_t。

#pragma once

#include "driver/gpio.h"

#if CONFIG_IDF_TARGET_ESP32C3

// ===========================================================================
// 上千 AI Passport · ESP32-C3 (QFN32, 8MB Flash, 无 PSRAM)
// ===========================================================================

#include "driver/spi_master.h"
#include "hal/adc_types.h"

// ---- 屏幕 ST7789P3 240x320（SPI2_HOST）----
// 仓库 bsp_pins.h:13-28：4 线 SPI、40MHz、mode0、出厂需反色。
#define BOARD_LCD_MOSI       GPIO_NUM_9
#define BOARD_LCD_SCLK       GPIO_NUM_8
#define BOARD_LCD_DC         GPIO_NUM_20
#define BOARD_LCD_CS         GPIO_NUM_1
// -1 = 复位脚未接 MCU（硬接 3.3V），由 esp_lcd_panel_reset() 走 SWRESET 软复位。
// 仓库 bsp_pins.h:21 明确写 (-1)。
#define BOARD_LCD_RST        GPIO_NUM_NC
#define BOARD_LCD_BL         GPIO_NUM_21
#define BOARD_LCD_H_RES      240
#define BOARD_LCD_V_RES      320
#define BOARD_LCD_PIXEL_CLK  (40 * 1000 * 1000)

// SPI 外设与模式：ST7789 SCK 空闲低、上升沿采样 → mode 0（仓库 bsp_pins.h:25）。
#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_LCD_SPI_MODE   0

// 背光 LEDC：20kHz 移出可听范围（5kHz 在非满占空比时会让升压电路嗡鸣）。
// 仓库 bsp_pins.h:32-37 用 TIMER_0 / CHANNEL_0；S3 用的是 1/1，两者互不影响。
#define BOARD_BL_LEDC_TIMER    LEDC_TIMER_0
#define BOARD_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BOARD_BL_FREQ_HZ       20000

// ---- 按键：三键共用一个 ADC 引脚，靠分压电阻区分 ----
// 电路：3.3V ── 外部上拉 10k ──┬── ADC 节点(GPIO0 / ADC1_CH0)
//                              └── 按键 ── 分压电阻 ── GND
//   上(UP)   : 0Ω    →   0 mV
//   下(DOWN) : 1k    → 300 mV
//   确定(OK) : 2.2k  → 595 mV
//   松开     : 上拉  → 3300 mV
// ⚠ 不能用内部上拉（~45kΩ 且精度差，三档会挤到 0~154mV 并重叠）。
// 仓库 bsp_pins.h:55-62。
#define BOARD_BTN_ADC_UNIT     ADC_UNIT_1
#define BOARD_BTN_ADC_CH       ADC_CHANNEL_0     // GPIO0
#define BOARD_BTN_ADC_ATTEN    ADC_ATTEN_DB_12   // 量程约 0~3100mV，覆盖松开态
#define BOARD_BTN_COUNT        3
// 每键电压窗口 {min_mV, max_mV}，边界取相邻档中点（12dB 衰减实测值）。
#define BOARD_BTN_MV_UP_MIN     0
#define BOARD_BTN_MV_UP_MAX     150
#define BOARD_BTN_MV_DOWN_MIN   150
#define BOARD_BTN_MV_DOWN_MAX   450
#define BOARD_BTN_MV_OK_MIN     450
#define BOARD_BTN_MV_OK_MAX     1000
// 松开判定：≥2500mV；1000~2500mV 是死区，按"未按键"处理。
#define BOARD_BTN_MV_RELEASED   2500

// ---- I2C：ES8311(0x18 音频 codec) 与 CW2017(0x63 电量计) 共用 ----
// 本轮只用到 CW2017；ES8311 初始化留到下一轮。
#define BOARD_I2C_PORT        I2C_NUM_0
#define BOARD_I2C_SDA         GPIO_NUM_10
#define BOARD_I2C_SCL         GPIO_NUM_7
#define BOARD_I2C_CW2017_ADDR 0x63

// ---- 音频 I2S（ES8311，16kHz 全双工）----
// 本轮只把引脚和 MCLK 接进 audio.cpp；codec 初始化不在本轮范围。
// 仓库 bsp_pins.h:76-83。
#define BOARD_MIC_WS         GPIO_NUM_3
#define BOARD_MIC_SCK        GPIO_NUM_5
#define BOARD_MIC_DIN        GPIO_NUM_4
#define BOARD_SPK_DOUT       GPIO_NUM_2
#define BOARD_SPK_BCLK       GPIO_NUM_5
#define BOARD_SPK_LRCK       GPIO_NUM_3
#define BOARD_SPK_MCLK       GPIO_NUM_6

// ---- 电源 ----
// 这块板没有充电检测 GPIO（电量走 CW2017 I2C），也没有独立电源键。
// battery_cw2017.cpp 通过 I2C 读 SOC；charging 恒为 false。
// 因此这里**不定义** BOARD_BAT_GPIO / BOARD_CHRG_GPIO。

#else  // ---------------- ESP32-S3（现有看板，行为保持不变）----------------

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

#include "driver/spi_master.h"
#include "hal/adc_types.h"

#define BOARD_LCD_SPI_HOST   SPI3_HOST
#define BOARD_LCD_SPI_MODE   3

// S3 沿用原来的 5kHz / TIMER_1 / CHANNEL_1 —— 这块屏的升压电路不响，
// 改频率属于无收益的行为变更，不动。
#define BOARD_BL_LEDC_TIMER    LEDC_TIMER_1
#define BOARD_BL_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BOARD_BL_FREQ_HZ       5000

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

#endif  // CONFIG_IDF_TARGET_ESP32C3
