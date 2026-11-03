// passport-fw/main/wb/wb_link.h —— BLE 外设（NimBLE）独立实现。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wb_link_frame_cb_t)(const char *frame, size_t len);

// 启动 BLE 外设：注册 WB GATT 服务并开始广播。只允许调用一次。
esp_err_t wb_link_start(wb_link_frame_cb_t on_frame);

// 发送一帧（自动按 MTU 切片，帧内切片不会被其它帧插入）。未连接时返回 false。
bool wb_link_send(const char *data, size_t len);

bool wb_link_connected(void);

// 待机/激活档位：切换连接间隔（约 247ms / 45ms），并对 Windows 主机做变快重试。
void wb_link_set_power_mode(bool low_power);
void wb_link_poll(void);

// 订阅成功后置位；应用侧在非 BLE 上下文里补发 hello。
bool wb_link_take_hello(void);

// 连接状态发生变化时返回 true，并给出当前状态。
bool wb_link_take_conn_change(bool *connected);

#ifdef __cplusplus
}
#endif
