/* 芯片温度 + 板载指标上报
 *
 * 每 30 秒 POST 一次 /device/report（芯片结温 / 电量 / 充电状态 / uptime）。
 * 从 main.cpp 拆出来的理由：它是一条独立的单向链路（设备 → 服务端），
 * 和看板取数（服务端 → 设备）互不影响，混在一个文件里容易误以为有关联。
 */
#pragma once

/* 上报周期。由 fetch 任务顺带节流 —— 不另起任务，
 * 是因为 POST 卡住时最坏只拖慢这一轮 GET（4 秒超时），下一轮照常。 */
#define DEVICE_REPORT_INTERVAL_MS  (30 * 1000)

/* 装温度传感器。失败不致命：页脚会显示 "--"。
 *
 * ESP32-S3 内置的是**射频用的**温度传感器，测的是芯片结温，
 * 不是环境温度 —— 芯片自己发热，读数天然比室温高 5~15°C。 */
void device_report_init(void);

/* 最近一次读到的芯片结温（°C），-1 = 传感器不可用。
 * UI 只读这个缓存，不直接调传感器：避免渲染任务和 fetch 任务
 * 同时操作同一个传感器句柄。 */
int device_report_chip_temp(void);

/* 采一次温度并上报。失败不重试 —— 30 秒后还有一轮，
 * 重试的边际价值几乎为零，反而可能拖住 fetch。 */
void device_report_send(void);
