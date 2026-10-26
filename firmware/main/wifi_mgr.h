/* WiFi 管理 + 手机配网
 *
 * ## 为什么要这层
 *
 * 原来 SSID/密码是编译期宏，换网络必须重新编译烧录。现在改成：
 *   · 凭据存 NVS，运行时配置，换 WiFi 不用重烧；
 *   · 连不上时屏幕上提示"长按 BOOT 3 秒配网"；
 *   · 长按进入配网模式，板子开热点，手机连上去打开网页填 WiFi。
 *
 * ## 凭据来源优先级
 *
 *   NVS 里存过  →  用 NVS 的
 *   NVS 没有    →  用编译期宏 WIFI_SSID / WIFI_PASSWORD（config.local.h）
 *   两者都没有  →  开机直接进配网模式，不空等
 *
 * ## 状态机
 *
 *   CONNECTING ──GOT_IP──> CONNECTED
 *        │                     │
 *        │重试 N 次仍失败       │断开
 *        ▼                     ▼
 *     FAILED <─────────────────┘
 *        │长按 BOOT 3 秒
 *        ▼
 *   PROVISION ──手机提交凭据──> 存 NVS ──> 重启 ──> CONNECTING
 *
 * 屏幕只认 state，判断口径集中在 wifi_mgr.cpp，改逻辑不用动 UI。
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_ST_CONNECTING = 0,   /* 正在连（开机就是这态） */
    WIFI_ST_CONNECTED,        /* 拿到 IP，可以轮询数据了 */
    WIFI_ST_FAILED,           /* 重试用尽，等用户处理 */
    WIFI_ST_PROVISION,        /* 配网模式：热点已开，HTTP 服务在跑 */
} wifi_state_t;

/* 读凭据并开始连接。没有凭据时直接进配网模式。
 * 必须在 nvs_flash_init() 之后调用。 */
void wifi_mgr_begin(void);

wifi_state_t wifi_mgr_state(void);

/* 供屏幕显示：当前（或待连）的 SSID。可能为空串。 */
const char *wifi_mgr_ssid(void);

/* 配网模式下：热点名、热点密码、配置页地址（如 192.168.4.1）。
 * 非配网模式返回空串。 */
const char *wifi_mgr_ap_ssid(void);
const char *wifi_mgr_ap_ip(void);
/* 当前连在配网热点上的手机数量（0 = 还没人连）。屏幕据此切换提示文案。 */
int         wifi_mgr_ap_clients(void);

/* 已连接时的本机 IP，未连接返回空串。 */
const char *wifi_mgr_ip(void);

/* FAILED 状态下手动重试一次（清计数、重新连）。 */
void wifi_mgr_retry(void);

/* 进入配网模式（开热点 + 起 HTTP 服务）。
 * 幂等：已经在配网模式时直接返回。 */
void wifi_mgr_start_provision(void);

/* 清掉 NVS 里的凭据并进入配网。返回 true 表示确实清掉了。 */
bool wifi_mgr_clear_creds(void);

#ifdef __cplusplus
}
#endif
