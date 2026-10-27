/* WiFi 管理 + 手机配网
 *
 * ## 为什么要这层
 *
 * 原来 SSID/密码是编译期宏，换网络必须重新编译烧录。现在改成：
 *   · 凭据存 NVS，运行时配置，换 WiFi 不用重烧；
 *   · 连不上时屏幕上提示"长按 BOOT 3 秒配网"；
 *   · 长按进入配网模式，板子开热点，手机连上去打开网页填 WiFi。
 *
 * ## 凭据来源
 *
 *   NVS 里存过  →  用 NVS 的
 *   NVS 没有    →  开机直接进配网模式，不空等
 *
 * ⚠ 2026-10-27 起**不再有编译期默认 WiFi**。
 * 原来还有 config.local.h 里的 WIFI_SSID / WIFI_PASSWORD 兜底，
 * 现在删掉了：烧一套默认密码既没用（换环境就得重烧）又是个泄密面，
 * 而手机配网页已经能覆盖这个需求。
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
 *
 * ## 服务地址（bridge base）
 *
 * 服务地址原来只有一个编译期宏 BRIDGE_BASE —— 电脑一换 IP（DHCP 下很常见），
 * 板子就连不上，只能改 configure.py 再重烧。现在它和 WiFi 凭据一样存 NVS：
 *   · 改完立刻生效，不用重烧、也不用重启；
 *   · 连续几次拉不到服务 → 在**已连上的 WiFi** 上开配置页，屏幕直接显示
 *     设备 IP，手机/电脑打开就能改；
 *   · 拉通一次后配置页自动关闭。
 *
 * 代码放在这里而不是新开一个模块：配置页就是同一个 httpd，拆出去要把
 * URL 解码 / 表单解析 / 内嵌页面整套复制一遍。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/* ------------------------------------------------------------------ 服务地址 */

/* 编译期兜底值（main.cpp 的 BRIDGE_BASE）。NVS 里配过时以 NVS 为准，
 * 所以这只是"一次都没配过"时的初值，不是硬编码。 */
void wifi_mgr_set_default_base(const char *base);

/* 当前生效的服务地址（形如 http://192.168.0.89:27777）。
 * fetch 每轮重新读，所以改完下一轮就生效。 */
const char *wifi_mgr_svr_base(void);

/* 地址每改动一次 +1。fetch 任务据此立刻重试，不用等下一个轮询周期。 */
uint32_t wifi_mgr_svr_version(void);

/* fetch 任务每轮上报成败。连续失败达到阈值就置 down 并自动开配置页。
 * WiFi 没连上时上报会被忽略 —— 那是 WiFi 的账，不能算到服务端头上。 */
void wifi_mgr_svr_report(bool ok);

/* WiFi 断了 / 正在配网：清掉失败计数（不是服务端的过错）。 */
void wifi_mgr_svr_reset(void);

/* 连续失败是否已达阈值。屏幕据此显示提示，配置页也据此开着。 */
bool wifi_mgr_svr_down(void);
int  wifi_mgr_svr_fails(void);

/* 本次上电后是否成功拉通过一次服务。
 * 屏幕用它区分"还在连"和"连上过又断了"：
 *   false + WiFi 已连 → "正在连接服务 + 服务器地址"
 *   true  + 连续失败   → 先维持旧数据，失败几次后才提示"服务连不上" */
bool wifi_mgr_svr_ever_ok(void);

/* 在**当前已连上的 WiFi** 上开配置页：不动 WiFi、不重启。
 * 返回配置页地址（设备自身 IP，如 "192.168.1.23"）；没连上 WiFi 返回空串。
 * 幂等：页面已经在跑就直接返回地址。 */
const char *wifi_mgr_start_svr_config(void);

/* 配置页验证码（4 位数字，有效期 5 分钟）。
 *
 * 配置页在"已连上 WiFi"时也是无鉴权的（跑在用户真实局域网上），
 * 所以改服务地址必须带上这个码：屏幕显示它，用户填进网页表单。
 * 配网模式（热点隔离网段）不需要，返回空串。 */
const char *wifi_mgr_cfg_token(void);

/* 当前验证码还剩几秒有效（1..300）。屏上倒计时用。
 * 到点会自动换新码，所以这里返回的总是"当前这枚码"的剩余时间。
 * 配网模式返回 0（那种模式不校验验证码）。 */
int wifi_mgr_cfg_token_left(void);

#ifdef __cplusplus
}
#endif
