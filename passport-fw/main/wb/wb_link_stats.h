// passport-fw/main/wb/wb_link_stats.h —— BLE 链路统计（纯数据 + 纯函数）。
//
// 为什么单独一个头文件：
//   wb_link.h 为了 esp_err_t 必须包含 IDF 头，而 wd_core.h（视觉层的状态快照）
//   与 wb_biz.h 都要引用这里的结构。抽出来以后，host 侧单测（cc 直接编
//   main/wb/*.c，没有 IDF 环境）也能 include 而不炸。
//
// 数据来源全部是设备侧自采（NimBLE GAP 回调 + 收发路径计数），不经过协议、
// 不依赖主机版本 —— 老主机也能看到「收帧 / 断连次数 / 连接间隔」。
// 这正是「关于页只有装饰」最大的缺口：以前 s_frames 无人累加、low_power 无人
// 设置、连接时长无来源，屏上只能写死「0 帧 / 正常 / 4fafc201」。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool connected;        // 当前是否已连接
    int64_t conn_since_ms; // 本段连接起点（毫秒，esp_timer 单调钟），0 = 未连接
    uint32_t rx_frames;    // 累计收到的完整帧数（本次上电以来）
    uint32_t tx_frames;    // 累计成功送出的帧数（按整帧计，不按 MTU 切片计）
    uint32_t disconnects;  // 累计断开次数（只数「连上后断掉」，连接失败不计）
    int last_reason;       // 最近一次断开原因，<0 = 本次上电还没断过
    uint16_t conn_itvl;    // 当前连接间隔，1.25ms 单位（BLE 规范原始单位）
    uint16_t mtu;          // 协商后的 ATT MTU（未协商时 23）
    bool low_power;        // 待机省电档位
} wb_link_stats_t;

// 1.25ms 单位 → 整数毫秒（itvl 36 = 45ms）。屏幕上给用户看的数。
static inline int wb_link_itvl_ms(uint16_t itvl_1_25)
{
    return (int)((itvl_1_25 * 5u + 3u) / 4u);
}

// 连接时长锚点文案（32px 大数字，宽度只有 124px 左右，7 个字符就到顶）。
// 分段而不是固定 HH:MM:SS：BLE 连接很少超过 1 小时，而 32px 下
// 「1:23:45」约 134px 会被截断成省略号，那比少显示小时更糟。
//   返回  0 → out 是「分:秒」MM:SS
//   返回  1 → out 是「时:分」HH:MM（不足 1 小时不出现这种形态）
//   返回 -1 → 未连接，out 是 "--:--"
// 调用方按返回值换说明文字（「分:秒」/「时:分」/「未连接」），
// 因此不会出现「42:17 被读成 42 小时」的歧义。
static inline int wb_link_fmt_clock(char *out, size_t cap, long long sec)
{
    if (!out || cap == 0) return -1;
    if (sec < 0) {
        snprintf(out, cap, "--:--");
        return -1;
    }
    if (sec < 3600) {
        snprintf(out, cap, "%02lld:%02lld", sec / 60, sec % 60);
        return 0;
    }
    snprintf(out, cap, "%02lld:%02lld", (sec / 3600) % 100, (sec / 60) % 60);
    return 1;
}

// 断开原因 → 中文短语。只翻最常踩的四个，其余保留十六进制：
// 屏上一行只有约 11 个汉字宽，穷举规范里 30 多个 reason 没有意义，
// 未知码照样打出来（配串口日志能查到），不编造。
// reason < 0 表示本次上电还没断过。
static inline void wb_link_fmt_reason(char *out, size_t cap, int reason)
{
    if (!out || cap == 0) return;
    if (reason < 0) {
        snprintf(out, cap, "无");
        return;
    }
    const char *why = NULL;
    switch (reason) {
    case 0x08: why = "连接超时"; break;
    case 0x13: why = "对端断开"; break;
    case 0x16: why = "本机断开"; break;
    case 0x3e: why = "建立失败"; break;
    default: break;
    }
    if (why) snprintf(out, cap, "%s", why);
    else snprintf(out, cap, "0x%02X", (unsigned)reason);
}

#ifdef __cplusplus
}
#endif
