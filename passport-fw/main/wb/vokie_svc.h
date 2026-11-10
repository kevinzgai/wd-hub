// passport-fw/main/wb/vokie_svc.h —— Vokie Passport BLE 语音服务（V1 协议）。
//
// 从 vokie-ai-passport-firmware 的 vokie_ble.c 移植合并进本固件：
// 服务 UUID 7f0e0001-6a7b-4b6f-9d1a-564f4b494500，Control/Audio/DeviceInfo
// 三个特征。与 wb_link 共存于同一个 GATT 服务器、支持双连接同时在线
// （wb_app 连看板 + Vokie 桌面端连语音）。协议契约见
// docs/ai-passport-ble-protocol.md（Vokie 仓库），主机端按服务 UUID 扫描。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_gap.h"

#ifdef __cplusplus
extern "C" {
#endif

// 设备状态（回调线程不固定，接收方自行同步）。
typedef enum {
    VOKIE_STS_DISCONNECTED = 0,
    VOKIE_STS_READY,
    VOKIE_STS_RECORDING,
    VOKIE_STS_PROCESSING,
    VOKIE_STS_SUCCESS,
    VOKIE_STS_ERROR,
} vokie_status_t;

// 在 nimble_port_init 之后、主机同步之前调用：注册 GATT 服务。
// 由 wb_link_start 内部调用，不要重复调用。
void vokie_svc_register_gatt(void);

// 创建音频采集/发送任务。wb_link_start 末尾调用。
void vokie_svc_start(void);

// 广播用：vokie 语音服务的 128 位 UUID（Vokie 主机按它扫描）。
const ble_uuid_t *vokie_svc_service_uuid(void);

// Vokie 主机完成握手（host_ready 且已订阅 Control/Audio）。
bool vokie_svc_host_ready(void);

// 正在录音（供按键层/提示音仲裁）。
bool vokie_svc_is_recording(void);

// GAP 事件转发入口：wb_link 收到 CONNECT/DISCONNECT/SUBSCRIBE/MTU/
// CONN_UPDATE/DATA_LEN_CHG 时调用，本模块按自己的连接/特征过滤。
void vokie_svc_on_gap_event(struct ble_gap_event *event);

// Vokie 模式下的按键（app_wb 路由）。btn 为 BSP_BTN_*，long_press=false
// 表示 CLICK。UP 点击 = PTT 开关；OK 点击 = 删字（录音中 = 取消）；
// DOWN 点击 = 回车；OK 长按 = 录音中取消（空闲时由上层用作退出模式）。
void vokie_svc_key(uint8_t btn, bool long_press);

// 强制停止当前会话（退出 Vokie 模式时）：录音中则走正常 ptt_up 收尾。
void vokie_svc_cancel(void);

// 状态回调（任意任务上下文）。
void vokie_svc_set_status_cb(void (*cb)(vokie_status_t st));

#ifdef __cplusplus
}
#endif
