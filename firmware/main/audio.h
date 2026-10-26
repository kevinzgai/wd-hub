// 提示音模块 —— 用 I2S 直驱功放播两声提示音。
//
// 硬件结论（已实测确认，见 .workbuddy/memory/2026-10-26.md）
// ------------------------------------------------------------
// 本机是 xingzhi-cube-1.54tft-wifi，上游用 NoAudioCodecSimplex：
// **没有 codec 芯片**，I2S 直接进功放芯片，所以不需要 I2C 初始化、
// 也不需要功放使能脚。
//   实测确认的引脚：BCLK=GPIO15  LRCK=GPIO16  DOUT=GPIO7
//   探测方法：写独立固件直接发正弦扫频音，用"能否听到"当探针
//   （没有示波器，声音本身就是通路证明，比查文档可靠）
//
// 为什么不存音频文件
// ------------------
// 波形在播放时实时合成，零 Flash 占用、零素材依赖。
// 两句提示音如果存成 24kHz PCM 要 70~100KB，而实时合成只占几百字节代码。
// 音色是正弦波，虽然没有真人语音自然，但作为"状态提示"完全够用，
// 而且音高/节奏可以代码调，不用重新录音。
//
// 线程模型
// --------
// audio_play() 只往队列里塞一个事件，立刻返回（非阻塞）。
// 真正的 I2S 写入在独立的 audio 任务里做，所以**绝不会**卡住
// LVGL 刷屏或 HTTP 轮询。这点很重要：一句提示音 300~400ms，
// 如果在 LVGL 任务里同步播，会掉十几帧。
#pragma once

#include <stdint.h>

// 播放事件。数值用于入队，不要随意改已有项的值。
typedef enum {
    AUDIO_EV_NONE = 0,
    AUDIO_EV_DONE,      // 完成一项 todo：上行三音，明亮圆满
    AUDIO_EV_ATTENTION, // 等你操作：下行双音，温和
    AUDIO_EV_CONNECTED, // 首次连上服务端：上行双音，短促上扬
    AUDIO_EV_SESSION_END, // 会话收工（working -1）：上行双音，比 DONE 略短
} audio_event_t;

// 初始化 I2S 与播放任务。必须在 app_main 里调一次。
// 失败只会打日志，不会 abort —— 没声音不应该让整个看板起不来。
void audio_init(void);

// 投递一个播放事件。非阻塞，队列满时直接丢弃（宁可少响一次，
// 也不要在这里阻塞调用方）。
void audio_play(audio_event_t ev);

// 音量百分比 0..100，默认 AUDIO_VOLUME_DEFAULT。
// 提示音固定音量，不占用按键（VOL_UP/DOWN 目前是调亮度的）。
void audio_set_volume(int percent);
