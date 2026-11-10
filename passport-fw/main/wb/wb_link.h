// passport-fw/main/wb/wb_link.h —— BLE 外设（NimBLE）独立实现。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "host/ble_gap.h"

#include "wb_link_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wb_link_frame_cb_t)(const char *frame, size_t len);

// GAP 事件转发回调：双连接下，wb_link 把 CONNECT / DISCONNECT / SUBSCRIBE /
// MTU / CONN_UPDATE 原样转发给第二套服务（vokie_svc），由对方按自己的
// 连接与特征过滤。必须在 wb_link_start 之前注册。
typedef void (*wb_link_gap_cb_t)(struct ble_gap_event *event);
void wb_link_set_gap_cb(wb_link_gap_cb_t cb);

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

// 链路统计快照（收帧 / 发帧 / 断连次数与原因 / 连接间隔 / MTU / 省电档）。
// 纯设备侧计数，不依赖主机协议版本。线程安全（无锁，逐字段读）。
void wb_link_get_stats(wb_link_stats_t *out);

#ifdef __cplusplus
}
#endif
