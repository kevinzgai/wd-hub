// main/wb_ble.h —— BLE GATT 外设:NimBLE 广播 + 单特征值收发。
//
// 与主机的契约(需求 v1.1 §6.1):
//   Service 4fafc201-1fb5-459e-8fcc-c5c9c331914b
//   Char    beb5483e-36e1-4688-b7f5-ea07361b26a8  (写 = 下行帧,notify = 上行帧)
//   设备名  Workbuddy BOX(主机按名字扫描,不按 service uuid)
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 收到一整帧(已按 '}' 或 '\n' 切好)时回调。运行在 NimBLE host 任务上下文,
// 只允许入队,不准阻塞、不准碰 LVGL。
typedef void (*wb_ble_frame_cb_t)(const char *frame, size_t len);

esp_err_t wb_ble_start(wb_ble_frame_cb_t on_frame);

bool wb_ble_connected(void);

// 发一帧(自动按 MTU 切片 notify)。链路未连接时返回 false。
bool wb_ble_send(const char *data, size_t len);

// 主机订阅了 notify 通道后置位;调用一次即清。用于连上之后补发 hello。
bool wb_ble_take_hello(void);

#ifdef __cplusplus
}  // extern "C"
#endif
