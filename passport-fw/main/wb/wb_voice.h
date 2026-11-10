// passport-fw/main/wb/wb_voice.h —— 按住 OK 说话：设备录音 → BLE 上行 → 主机
// 虚拟声卡 → 微信输入法听写（参考 voice-notes 的「设备听写」交互）。
//
// 协议契约在主机 host/wb_app.py：
//   * 主机按 {"voice":"poll"} 轮询，设备每次 poll 回一行：
//     {"voice":1,"session":N,"recording":bool,"sequence":N,"pending":N,"peak":N,
//      "dropped":N,"stop_reason":N,"clipped":N,"samples":N,"adpcm":"<base64>"}
//   * adpcm = 若干 163B IMA 帧（20ms/帧 @16kHz），与主机 _adpcm_decode 逐字节互逆；
//   * sequence 是本批首帧的序号；recording 的下降沿 + 排空 = 录音结束；
//   * 开录瞬间设备**主动**发一行（不等 poll），让主机尽快按下听写热键。
//
// 触发：长按 OK 开始（app_wb），松开结束。录音期间 main.cc 抑制
// 「长按 OK 2s 关机提示 / 5s 关机」手势，否则说话超 2 秒就弹关机提示。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 请求开始一次录音会话（在按键上下文调用，绝不阻塞；真正开 codec 在
// voice 任务里做）。空闲且 BLE 已连接时返回 true 并立即进入「激活」态
// —— main.cc 的关机手势抑制依赖这个标志立刻生效。
bool wb_voice_request_start(void);

// 请求结束当前会话（正常松开，stop_reason=1）。未激活时是空操作。
void wb_voice_request_stop(void);

// 会话是否激活（含正在排空尾巴的阶段）。main.cc 用它抑制关机手势。
bool wb_voice_is_active(void);

// 主机 poll 一次（wb_task/BLE 上下文调用）：回一行语音数据。
// 空闲时是空操作 —— 主机 caps 声明 bit5 后会 1Hz 空轮询。
void wb_voice_on_poll(void);

#ifdef __cplusplus
}
#endif
