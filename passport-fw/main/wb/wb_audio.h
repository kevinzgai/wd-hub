// passport-fw/main/wb/wb_audio.h —— WB 提示音（16kHz 单声道素材，异步队列播放）。
//
// 与 S3 看板固件的分工一致：音频数据来自同一段素材（复用到本工程，
// 不链接 firmware/ 源码），但播放通道由本工程独立实现。
//
// 内存红线：C3 无 PSRAM，内部 RAM 最大连续块约 114KB。本模块只申请
// 400ms 的立体声展开缓冲（16000 × 0.4 × 2ch × 2B = 25,600B），
// 分块写入，最长 702ms 的提示音用两块即可播完。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_SFX_NONE = 0,
    WB_SFX_ATTENTION,   // 新审批 / 新提问 / 低电量
    WB_SFX_DONE,        // 任务完成
    WB_SFX_CONNECTED,   // 新任务 / 连接测试 / 审批允许
    WB_SFX_END,         // 会话收工
    WB_SFX_KEY,         // 按键短反馈 / 审批拒绝
} wb_sfx_t;

// 分配播放缓冲 + 队列 + 播放任务。codec 在第一次播放时才打开，
// 避免开机音（boot_chime）仍持有 codec 时初始化失败。
bool wb_audio_init(void);

// 停止任务、关闭 codec、释放缓冲。应用退出时必须调用。
void wb_audio_deinit(void);

// 投递一个提示音；队列满立即丢弃，绝不阻塞调用方（可能在 LVGL/BLE 任务里）。
void wb_audio_play(wb_sfx_t sfx);

// 待机闸门：待机时丢弃所有提示音；退出待机后恢复。
void wb_audio_set_standby(bool standby);

// 主机静音（cfg.beep_vol <= 0）。与 S3 一致：只静音通知类提示音，
// 按键反馈不受影响（S3 的按键音在按键驱动里，独立于 beep_vol）。
void wb_audio_set_host_mute(bool mute);

// 低电量提示。阈值 15%，回差 18%；待机中先挂起，唤醒后补响一次。
void wb_audio_low_battery(int soc);

// 串口诊断：请求 / 播放 / 丢弃 / 失败计数 + codec 状态。
void wb_audio_log_stats(void);

#ifdef __cplusplus
}
#endif
