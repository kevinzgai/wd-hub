// 电池电量读取（GPIO17 / ADC2_CH6）+ 充电状态（GPIO38）
//
// 来源：上游 78/xiaozhi-esp32
//       main/boards/nologo/xingzhi-cube-1.54tft-wifi/power_manager.h
//
//   adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_2 };
//   adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_6, &chan_config);
//   gpio_get_level(charging_pin_) == 1  -> 充电中
//
// IDF soc/esp32s3/adc_channel.h: #define ADC2_CHANNEL_6_GPIO_NUM 17
//
// 上游分段表（12bit raw）：
//   1970→0%  2062→20%  2154→40%  2246→60%  2338→80%  2430→100%
// 各段线性插值。
//
// ⚠️ 本模块只碰 GPIO17(ADC) 与 GPIO38(输入)，不触碰任何 I2S 音频脚。
//
// ── 关于"充电时电量虚高"（重要，不是 bug）──────────────────────────
// 电压法测电量的固有缺陷：充电时充电 IC 以恒流把电池**端电压**顶到 4.2V
// 上限，这个值不代表真实剩余电量。拔掉后电压回落到开路电压(OCV)才是真值。
// 表现就是：充电时恒显 100%，一拔线立刻掉到 50~60%。
//
// 处理策略（三段式，见 battery_sample_once）：
//   1. 放电中 —— 直接用读数换算，这是可信的
//   2. 充电中 —— 用"充电前的最后一次可信值"作为显示值，不让它跳到 100%
//   3. 充电中但从没测过（插着电开机）—— 按端电压减 CC_CV_DROP 粗估
//
// 这样充电全程看到的都是"真实的剩余电量"，充到饱也不虚报。

#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int  raw;          // 最近一次平均 raw 值
    int  mv;           // 换算电压，-1 = 校准不可用
    int  percent;      // 0..100，-1 = 尚未采到有效值。充电时为"真实电量"估值
    int  volt_pct;     // 纯按端电压换算的百分比（充电时会虚高，仅供调试）
    bool charging;     // GPIO38 高电平
    bool percent_held; // true = percent 来自"充电前快照"而非当前电压
    bool valid;        // 至少采到过一次
} battery_state_t;

// 初始化 ADC + 充电脚，并启动周期采样任务
void battery_init(void);

// 取当前快照（任意任务可调用，内部有锁）
void battery_get(battery_state_t *out);

// 把 raw 值按上游分段表换算成百分比（供单元测试直接调用）
int battery_raw_to_percent(int raw);
