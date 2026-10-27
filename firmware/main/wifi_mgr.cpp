/* WiFi 管理 + 手机配网 —— 实现
 *
 * 详细的状态机说明见 wifi_mgr.h。
 *
 * 几个容易踩的点，写在前面：
 *
 * 1) **不要在事件回调里 vTaskDelay**。旧代码在 WIFI_EVENT_STA_DISCONNECTED
 *    回调里直接延时 2s 再重连，那会把默认事件循环任务卡住（它是单线程的），
 *    期间所有事件都出不来。这里改用一次性 esp_timer 调度重连。
 *
 * 2) **配网时不能让它继续自动重连**。否则屏幕上写着"热点已开"，后台还在
 *    拿旧密码狂连，日志刷屏且耗电。用 s_suppress_reconnect 抑制。
 *
 * 3) **凭据要先进 NVS 再重启**，并且在重启前把 NVS 刷盘。esp_restart() 是
 *    硬重启，nvs_flash 内部有缓存，不 commit 会丢掉刚写的东西。
 */

#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"

/* 注意：**不再**从这里读 WIFI_SSID / WIFI_PASSWORD。
 *
 * 2026-10-27 起取消编译期默认 WiFi：凭据只认 NVS，没有就进配网。
 * 理由：固件里烧一套默认密码既没用（换环境就得重烧）又是个泄密面，
 * 而手机配网已经能覆盖这个需求。
 * （config.local.h 仍保留，main.cpp 用它拿 BRIDGE_BASE 服务地址。） */

static const char *TAG = "wifi";

/* ---------------------------------------------------------------- 参数 */

/* 这个命名空间现在装两样东西：WiFi 凭据 + 服务地址。
 * 合在一个 ns 里是为了少一次 nvs_open，也因为它们的生命周期一致
 * （都是"配过一次就一直有效"），没必要分家。 */
#define NVS_NS          "wbwifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define NVS_KEY_BASE    "base"      /* 服务地址 BRIDGE_BASE */

/* 服务地址最大长度。"https://" + 253 主机名太夸张，实际只会是 IP:端口，
 * 64 足够，也正好是 fetch 里拼 URL 的缓冲的一半。 */
#define SVR_BASE_MAX    64

/* 连续拉不到服务几次就认定"服务地址不对了"。
 *
 * 取值考虑：轮询间隔 5s，失败 3 次后拉长到 15s，所以第 5 次失败大约在
 * 开机/断连后 30 秒。太早（1~2 次）会把电脑休眠、服务重启这种几十秒的
 * 抖动误判成"地址变了"；太晚用户已经盯着"离线"看了好几分钟。30 秒是
 * "确实是地址的问题"和"别太烦人"之间的折中。 */
#define SVR_FAIL_MAX    5

/* 配网热点做成**开放网络**（不要密码）。
 *
 * 理由：这热点只在配网时存在 3 分钟，作用是让用户连上来打开配置页，
 * 本身不承载任何敏感数据；而打密码意味着用户得在 240x240 的小屏上
 * 逐字念一串 wb12345678，输错一次就连不上，纯粹加摩擦。
 * 安全性由"配网窗口只有 3 分钟 + 保存后立即重启"兜住。 */

/* 连不上重试几次就放弃、转到 FAILED。
 * 4 次 × 4s ≈ 16s，用户开机能等到，又不会无限等。 */
#define MAX_RETRY       4
#define RETRY_DELAY_MS  4000

/* 保存成功后延时重启，给浏览器一点时间收到响应 */
#define REBOOT_DELAY_MS 1500

/* ---------------------------------------------------------------- 配置页鉴权
 *
 * ⚠ 为什么需要：配置页是**无鉴权**的 HTTP 服务，而它在"已连上 WiFi"的
 * 状态下也会开着（服务连不上时自动开）。同一个局域网里任何设备访问
 * http://<设备IP>/ 就能：
 *   - 从 /info 读到你的 WiFi 名称（SSID）
 *   - 调 /setbase 把服务地址改成自己的机器，之后屏上显示的
 *     "工作状态"全是伪造的
 *
 * 方案：屏幕显示一个 4 位数字码，页面提交时必须带上。对策两个：
 *   1. 配网热点本身是隔离网段（192.168.4.1），只有连着热点的手机能访问，
 *      风险低 —— 所以配网模式**不要求**验证码；
 *   2. "已连 WiFi"模式跑在用户的真实局域网里，任何人都能访问 —— 这个
 *      模式**必须**校验验证码。
 *
 * 验证码每 30 秒换一次。**唯一的获取渠道是设备屏幕** —— 用户看着屏上的
 * 4 位数字手抄进表单。/info 绝不下发它（那等于把钥匙和锁挂在一扇门上）。 */
#define CFG_TOKEN_LEN   4
#define CFG_TOKEN_TTL_MS 30000

/* 配网模式无人搭理就自动重启回正常。
 *
 * 为什么需要：BOOT 是 GPIO0，也就是 ESP32-S3 的启动 strapping 脚，
 * 板上自动复位电路会在串口 DTR 拉低时把它带下去 —— 实测用工具打开
 * 串口就会误判成"BOOT 被长按 3 秒"，把设备骗进配网模式。
 * 进了配网就停止轮询数据，屏幕上只剩配网提示，用户会以为板子坏了。
 *
 * 有了这个超时，最坏情况也只是"卡 3 分钟"，之后自己回到正常工作。
 * 每次 HTTP 请求都会重置计时，所以真正在配网的人不会被踢掉。 */
#define PROV_IDLE_TIMEOUT_MS (3 * 60 * 1000)

/* ---------------------------------------------------------------- 状态 */

static volatile wifi_state_t s_state = WIFI_ST_CONNECTING;
static volatile int          s_fail_count;
static volatile bool         s_suppress_reconnect;

static char s_ssid[33];
static char s_ip[16];
static char s_ap_ssid[33];
static char s_ap_ip[16];

/* ---- 服务地址 ---- */
static char            s_svr_base[SVR_BASE_MAX];     /* 当前生效 */
static char            s_svr_default[SVR_BASE_MAX];  /* 编译期 BRIDGE_BASE 兜底 */
static volatile int    s_svr_fails;                  /* 连续失败次数 */
static volatile bool   s_svr_down;                   /* 已达阈值：屏幕提示 + 开配置页 */
static volatile uint32_t s_svr_ver;                  /* 每次改动 +1，fetch 据此立刻重试 */
/* 配置页是在"已连着 WiFi"的状态下开的（区别于配网热点的那个）。
 * 决定了服务恢复时要不要 httpd_stop —— 配网模式的页面是等重启来关的。 */
static volatile bool   s_svr_http;

/* 已连到配网热点的手机数量。
 * 屏幕文案要跟着它变：没人连时提示"手机连这个热点"，
 * 连上之后改提示"打开 192.168.4.1"——用户才知道下一步做什么。 */
static volatile int s_ap_clients;

static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_reboot_timer;
static esp_timer_handle_t s_prov_timer;
static httpd_handle_t     s_httpd;

/* ⚠ 配网页提交上来的凭据。
 * 原来这里是两个静态 buffer，但 h_save 是 HTTP handler，esp_http_server
 * 默认 max_open_sockets=7，**多个请求可以并发进入不同任务**。
 * 两个请求同时写 s_new_ssid / s_new_pass，就是数据竞争：
 * A 请求写完 ssid，B 请求把 ssid 覆盖掉，A 随后用 B 的 ssid 去存 NVS。
 * 结果是"用户明明填了自己的 WiFi，设备连上了别人的"。
 * 改成 handler 内的局部变量：栈上各有一份，天然隔离。
 * 98 字节 @ stack 6144，完全放得下。 */

/* 配置页验证码。当前值 + 生成时刻。 */
static char          s_cfg_token[CFG_TOKEN_LEN + 1];
static uint32_t      s_cfg_token_ms;

/* ---------------------------------------------------------------- NVS */

static bool creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    bool ok = false;
    size_t n = ssid_len;
    if (nvs_get_str(h, NVS_KEY_SSID, ssid, &n) == ESP_OK && ssid[0]) {
        n = pass_len;
        /* 密码允许为空（开放网络），取不到就当空串 */
        if (nvs_get_str(h, NVS_KEY_PASS, pass, &n) != ESP_OK) pass[0] = '\0';
        ok = true;
    }
    nvs_close(h);
    return ok;
}

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool creds_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_erase_key(h, NVS_KEY_SSID);
    esp_err_t e2 = nvs_erase_key(h, NVS_KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
    return (e1 == ESP_OK || e2 == ESP_OK);
}

/* ---------------------------------------------------------------- 服务地址 */

/* 归一化：去首尾空白、去掉结尾的 /（拼路径时固件会自己加）、
 * 没写协议就补 http://。
 *
 * 为什么容忍"只填 IP:端口"：服务地址最常改的部分就是 IP，让用户每次都
 * 手打 http:// 纯属摩擦；而结尾多一个 / 会让 URL 变成 //status，
 * 服务端不认，所以也要一并收拾掉。 */
static void svr_normalize(const char *in, char *out, size_t out_len)
{
    while (*in == ' ') in++;

    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\r' || in[n - 1] == '\n')) n--;

    /* 给 http:// 留 7 字节：手拼而不是 snprintf 是因为 snprintf 的 %s 会让
     * 编译器算不准上界，在 -Werror=format-truncation 下直接报错。
     * 这里拼的两段长度都是自己算出来的，截断时也是显式截断。 */
    char tmp[SVR_BASE_MAX - 8];
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, in, n);
    tmp[n] = '\0';

    while (n && tmp[n - 1] == '/') tmp[--n] = '\0';

    bool   need   = (strncmp(tmp, "http://", 7) != 0 &&
                     strncmp(tmp, "https://", 8) != 0);
    const char *pre = need ? "http://" : "";
    size_t plen = strlen(pre);

    if (out_len <= plen) { out[0] = '\0'; return; }
    if (n > out_len - plen - 1) n = out_len - plen - 1;

    memcpy(out, pre, plen);
    memcpy(out + plen, tmp, n);
    out[plen + n] = '\0';
}

/* 开机时定下生效值：NVS 配过用 NVS，否则用编译期 BRIDGE_BASE。 */
static void svr_load(void)
{
    char raw[SVR_BASE_MAX] = {};
    bool have = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(raw);
        if (nvs_get_str(h, NVS_KEY_BASE, raw, &n) == ESP_OK && raw[0]) have = true;
        nvs_close(h);
    }

    if (have) {
        svr_normalize(raw, s_svr_base, sizeof(s_svr_base));
        ESP_LOGI(TAG, "服务地址 %s（来自 NVS）", s_svr_base);
    } else if (s_svr_default[0]) {
        strncpy(s_svr_base, s_svr_default, sizeof(s_svr_base) - 1);
        s_svr_base[sizeof(s_svr_base) - 1] = '\0';
        ESP_LOGI(TAG, "服务地址 %s（编译期默认，可在配置页改）", s_svr_base);
    } else {
        s_svr_base[0] = '\0';
        ESP_LOGW(TAG, "没有任何服务地址，等配置页写入");
    }
}

static esp_err_t svr_save(const char *in)
{
    char norm[SVR_BASE_MAX];
    svr_normalize(in, norm, sizeof(norm));
    if (!norm[0] || strcmp(norm, "http://") == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_BASE, norm);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "服务地址保存失败: %s", esp_err_to_name(err));
        return err;
    }

    strncpy(s_svr_base, norm, sizeof(s_svr_base) - 1);
    s_svr_base[sizeof(s_svr_base) - 1] = '\0';
    s_svr_ver = s_svr_ver + 1;

    /* 只清失败计数，不动 down：改完地址还没验证过，屏幕提示继续留着，
     * 直到真的拉通一次才消失。否则"改了但还是错"的时候提示会闪一下又回来，
     * 用户会以为改成功了。 */
    s_svr_fails = 0;
    ESP_LOGW(TAG, "服务地址改为 %s", s_svr_base);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 重连调度 */

static void retry_timer_cb(void *arg)
{
    if (s_suppress_reconnect) return;
    if (s_state == WIFI_ST_PROVISION) return;
    ESP_LOGI(TAG, "重连（第 %d/%d 次）", s_fail_count + 1, MAX_RETRY);
    esp_wifi_connect();
}

static void reboot_timer_cb(void *arg)
{
    ESP_LOGW(TAG, "配置已保存，重启生效");
    esp_restart();
}

/* 配网长时间没人访问 —— 多半是误触发进来的，重启回正常工作。
 *
 * ⚠ 只在**已经存过凭据**时才重启。一次都没配过的设备本来就该停在配网
 * 模式等用户配置，重启只会变成"开机→配网→3 分钟→重启→配网"的死循环。
 */
static void prov_timeout_cb(void *arg)
{
    char ssid[33] = {};
    char pass[65] = {};
    if (!creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "配网空闲超时，但没有任何已保存的凭据 —— 继续等待配置");
        return;
    }
    ESP_LOGW(TAG, "配网 %d 秒无人访问，自动重启回到正常工作",
             PROV_IDLE_TIMEOUT_MS / 1000);
    esp_restart();
}

/* 每次 HTTP 请求都推迟超时。真正在配网的人不会被中途踢掉。
 *
 * ⚠ 只在配网模式下计时。这个页面现在有两种打开方式：配网热点（要超时重启
 * 兜底）和"已连 WiFi 时改服务地址"（页面开着期间设备照常工作，不该被
 * 定时器重启）。不加这道判断，改服务地址改到 3 分钟就会把设备重启掉。 */
static void prov_keepalive(void)
{
    if (!s_prov_timer) return;
    if (s_state != WIFI_ST_PROVISION) return;
    esp_timer_stop(s_prov_timer);
    esp_timer_start_once(s_prov_timer, PROV_IDLE_TIMEOUT_MS * 1000);
}

/* ---------------------------------------------------------------- 事件 */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_suppress_reconnect) esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_suppress_reconnect || s_state == WIFI_ST_PROVISION) return;

        s_fail_count = s_fail_count + 1;    /* volatile 不能用 ++（C++20 弃用） */
        if (s_fail_count >= MAX_RETRY) {
            /* 不再重连，等用户按 BOOT 进配网。屏幕会切到提示界面。 */
            s_state = WIFI_ST_FAILED;
            ESP_LOGW(TAG, "连不上 %s（已试 %d 次），请长按 BOOT 3 秒重新配网",
                     s_ssid, s_fail_count);
            return;
        }
        s_state = WIFI_ST_CONNECTING;
        /* 用 timer 而不是在这里 vTaskDelay —— 见文件头第 1 条 */
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, RETRY_DELAY_MS * 1000);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_fail_count = 0;
        s_state = WIFI_ST_CONNECTED;
        ESP_LOGI(TAG, "已连接 %s，IP %s", s_ssid, s_ip);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        /* 手机连上配网热点了 —— 屏幕要立刻改提示 */
        s_ap_clients = s_ap_clients + 1;
        if (s_ap_clients > 4) s_ap_clients = 4;
        ESP_LOGI(TAG, "手机已连上配网热点（共 %d 台）", s_ap_clients);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients = s_ap_clients - 1;
        ESP_LOGI(TAG, "手机断开配网热点（剩 %d 台）", s_ap_clients);
    }
}

/* ---------------------------------------------------------------- 配网 HTTP */

/* URL 解码（%XX 和 +）。就地处理，target 长度不会变长。 */
static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* ---------------------------------------------------------------- 配置页验证码 */

/* form_get 的定义在下面（本段之后），这里先声明。
 * 不把它挪到一块是因为验证码这几个函数和 form_get 都只服务 HTTP 层，
 * 按"验证码 → 表单解析 → 页面"的顺序排更顺；但 form_get 用到的
 * url_decode 在上面，所以只能这样前后各放一半。 */
static bool form_get(const char *body, const char *key, char *out, size_t out_len);

/* 生成/返回当前验证码。TTL 到了就换一个。
 *
 * ⚠ 随机源用 esp_random()，不用 rand()：rand() 的种子在 ESP-IDF 里
 * 每次启动都一样（除非显式 srandom）， predictable 的验证码等于没有。 */
static const char *cfg_token(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_cfg_token[0] &&
        (uint32_t)(now - s_cfg_token_ms) < CFG_TOKEN_TTL_MS) {
        return s_cfg_token;
    }

    uint32_t r = esp_random();
    for (int i = 0; i < CFG_TOKEN_LEN; i++) {
        s_cfg_token[i] = (char)('0' + (r % 10));
        r /= 10;
    }
    s_cfg_token[CFG_TOKEN_LEN] = '\0';
    s_cfg_token_ms = now;
    ESP_LOGI(TAG, "配置页验证码: %s", s_cfg_token);
    return s_cfg_token;
}

/* 配置页验证码。屏幕要显示它，用户才能填进网页表单。
 * 配网模式不校验，不需要显示。 */
const char *wifi_mgr_cfg_token(void)
{
    if (s_state == WIFI_ST_PROVISION) return "";
    return cfg_token();
}

/* 校验请求带上的验证码。
 *
 * 配网模式（热点网段）**不校验**：那台手机本来就站在设备面前，
 * 还要它从 240px 小屏上抄 4 位数字，纯属摩擦。
 * "已连 WiFi"模式（跑在真实局域网里）**必须校验**：那儿谁都能访问。 */
static bool cfg_token_ok(httpd_req_t *req)
{
    if (s_state == WIFI_ST_PROVISION) return true;

    /* 令牌从 query 取：页面是 GET 拿页面 + POST 提交，
     * 表单里塞一个隐藏字段也行，但 query 更简单，不用改 HTML 结构。 */
    char q[64] = {};
    size_t ql = httpd_req_get_url_query_len(req);
    if (ql == 0 || ql >= sizeof(q)) return false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;

    char given[16] = {};
    if (!form_get(q, "t", given, sizeof(given))) return false;

    const char *cur = cfg_token();
    if (strlen(given) != CFG_TOKEN_LEN) return false;
    /* 令牌是纯数字，逐字符比。别用 strcasecmp —— 数字没有大小写，
     * 而且 Windows 版 ESP-IDF 未必有那个符号。 */
    for (int i = 0; i < CFG_TOKEN_LEN; i++) {
        if (given[i] != cur[i]) return false;
    }
    return true;
}

/* 从 a=1&b=2 里取 key 的值，解到 out。找到返回 true。 */
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            if (n >= out_len) n = out_len - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* JSON 字符串转义：SSID 里可能有引号/反斜杠/中文，直接塞会破坏 JSON */
static size_t json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 7 < dst_len; p++) {
        if (*p == '"' || *p == '\\') { dst[o++] = '\\'; dst[o++] = (char)*p; }
        else if (*p < 0x20)          { o += snprintf(dst + o, dst_len - o, "\\u%04x", *p); }
        else                          { dst[o++] = (char)*p; }
    }
    dst[o] = '\0';
    return o;
}

static const char k_config_html[] =
R"HTML(<!DOCTYPE html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>看板配置</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font:15px/1.6 -apple-system,system-ui,"PingFang SC",sans-serif;
background:#0d1117;color:#e6edf3;padding:20px 16px 40px;max-width:520px;margin:0 auto}
h1{font-size:18px;font-weight:600;margin-bottom:4px}
.sub{color:#8b949e;font-size:12.5px;margin-bottom:20px}
.card{background:#161b22;border:1px solid #21262d;border-radius:12px;padding:16px;margin-bottom:14px}
label{display:block;font-size:12.5px;color:#8b949e;margin-bottom:6px}
input{width:100%;background:#0d1117;border:1px solid #30363d;border-radius:8px;
padding:11px 12px;color:#e6edf3;font-size:15px;outline:none}
input:focus{border-color:#4a9eff}
button{width:100%;border:0;border-radius:8px;padding:12px;font-size:15px;
font-weight:500;cursor:pointer;margin-top:10px}
.primary{background:#2f81f7;color:#fff}
.primary:disabled{background:#21262d;color:#6e7681;cursor:default}
.ghost{background:#21262d;color:#c9d1d9}
#list{max-height:220px;overflow:auto;margin-top:10px}
.net{display:flex;justify-content:space-between;align-items:center;
padding:11px 12px;border-radius:8px;cursor:pointer;font-size:14px}
.net:active{background:#21262d}
.net .r{color:#8b949e;font-size:12px;flex:none;margin-left:10px}
.net.lock .n::after{content:" 🔒";font-size:11px}
.msg{margin-top:12px;font-size:13px;padding:10px 12px;border-radius:8px;display:none}
.msg.ok{display:block;background:#0f2a17;color:#4fd463;border:1px solid #1e5c2e}
.msg.err{display:block;background:#2d1214;color:#ff7b72;border:1px solid #5c1e22}
.tip{font-size:12px;color:#6e7681;margin-top:10px;line-height:1.5}
</style></head><body>
<h1 id="head">WorkBuddy 看板</h1>
<p class="sub" id="sub">加载中…</p>

<div class="card" id="wifiCard">
  <label>可用的 WiFi</label>
  <button class="ghost" id="scan">重新扫描</button>
  <div id="list"><div class="tip">扫描中…</div></div>

  <div style="margin-top:16px">
    <label>WiFi 名称（SSID）</label>
    <input id="ssid" placeholder="点上面选，或直接输入" autocapitalize="off" autocorrect="off">
    <label style="margin-top:14px">密码</label>
    <input id="pass" type="password" placeholder="开放网络留空">
    <button class="primary" id="save">保存并连接</button>
    <div class="msg" id="msg"></div>
    <p class="tip">保存后设备会自动重启去连新 WiFi。<br>
    连不上时长按板上 BOOT 键 3 秒可再次进入本页面。</p>
  </div>
</div>

<div class="card">
  <label>服务地址（电脑 IP:端口）</label>
  <input id="base" placeholder="192.168.0.89:27777" autocapitalize="off" autocorrect="off" spellcheck="false">
  <label id="tokLabel" style="margin-top:14px;display:none">设备屏幕上的 4 位验证码</label>
  <input id="token" placeholder="屏幕上那 4 位数字" inputmode="numeric" maxlength="4" style="display:none">
  <button class="primary" id="saveBase">保存并立即生效</button>
  <div class="msg" id="msg2"></div>
  <p class="tip">电脑换 IP 不用重烧固件：改这里，看板几秒内自己连上新地址。<br>
  端口固定 27777，通常只改 IP。不写 http:// 也认。</p>
</div>

<script>
const $=id=>document.getElementById(id);
/* 已连 WiFi 模式（svr）需要验证码；配网模式（wifi）不需要。 */
let needToken=false;
function showMsg(id,t,ok){const m=$(id);m.textContent=t;m.className='msg '+(ok?'ok':'err');}
async function info(){const r=await fetch('/info',{cache:'no-store'});return await r.json();}
async function scan(){
  $('list').innerHTML='<div class="tip">扫描中…</div>';
  try{
    const r=await fetch('/scan',{cache:'no-store'});
    const a=await r.json();
    if(!a.length){$('list').innerHTML='<div class="tip">没扫到网络，靠近路由器再试</div>';return;}
    $('list').innerHTML=a.map(n=>{
      const bars=n.rssi>-55?'▂▄▆█':n.rssi>-70?'▂▄▆':'▂▄';
      return `<div class="net${n.auth?' lock':''}" data-s="${encodeURIComponent(n.ssid)}">
        <span class="n"></span><span class="r">${bars}</span></div>`;
    }).join('');
    $('list').querySelectorAll('.net').forEach((el,i)=>{
      el.querySelector('.n').textContent=a[i].ssid;
      el.onclick=()=>{$('ssid').value=a[i].ssid;$('pass').focus();};
    });
  }catch(e){$('list').innerHTML='<div class="tip">扫描失败</div>';}
}
$('scan').onclick=scan;
$('save').onclick=async()=>{
  const ssid=$('ssid').value.trim();
  if(!ssid){showMsg('msg','请先选一个 WiFi',false);return;}
  $('save').disabled=true;$('save').textContent='保存中…';
  try{
    const r=await fetch('/save',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('pass').value)});
    if(r.ok){showMsg('msg','已保存，设备重启中…请稍候',true);}
    else{showMsg('msg','保存失败，请重试',false);$('save').disabled=false;$('save').textContent='保存并连接';}
  }catch(e){showMsg('msg','保存失败：'+e.message,false);$('save').disabled=false;$('save').textContent='保存并连接';}
};

/* 服务地址：保存后不重启，看板下一轮轮询就用新地址。
 * 这里轮询 /info 看有没有连上 —— 连上后设备会把配置页关掉，
 * 那时 fetch 会失败，正好当作"已连上"处理。
 *
 * ⚠ 已连 WiFi 模式（svr）要带验证码：这个页面跑在用户的真实局域网上，
 * 同网段任何设备都能访问，不校验的话别人能把看板的服务地址改成
 * 自己的机器，屏上的"工作状态"就全是假的。 */
$('saveBase').onclick=async()=>{
  const base=$('base').value.trim();
  const b=$('saveBase');
  if(!base){showMsg('msg2','请填服务地址',false);return;}
  const tok=$('token').value.trim();
  if(needToken&&!tok){showMsg('msg2','请填屏幕上那 4 位验证码',false);return;}
  b.disabled=true;b.textContent='保存中…';
  try{
    const r=await fetch('/setbase'+(needToken?('?t='+encodeURIComponent(tok)):''),{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'base='+encodeURIComponent(base)});
    if(!r.ok){showMsg('msg2','保存失败：验证码不对或地址无效',false);b.disabled=false;b.textContent='保存并立即生效';return;}
    showMsg('msg2','已保存，正在按新地址重连…',true);
    let n=0;
    const t=setInterval(async()=>{
      n++;
      try{
        const d=await info();
        if(!d.down){clearInterval(t);showMsg('msg2','已连上新地址，屏幕提示会自动消失',true);b.disabled=false;b.textContent='保存并立即生效';}
        else if(n>=10){clearInterval(t);showMsg('msg2','还是连不上：确认电脑上服务已启动、IP 没填错',false);b.disabled=false;b.textContent='保存并立即生效';}
      }catch(e){
        clearInterval(t);
        showMsg('msg2','已连上，配置页已关闭',true);
      }
    },2000);
  }catch(e){showMsg('msg2','保存失败：'+e.message,false);b.disabled=false;b.textContent='保存并立即生效';}
};

/* 两种打开方式共用一个页面：
 *   wifi = 配网热点里开的（能扫 WiFi，重点在配网）
 *   svr  = 已经连着 WiFi、只是服务地址不对（不用扫 WiFi，重点在改地址）
 *
 * ⚠ svr 模式跑在用户的真实局域网上，必须校验屏幕上那 4 位验证码。
 * 配网模式不校验 —— 热点是隔离网段，能连上的人就站在设备面前。 */
(async()=>{
  try{
    const d=await info();
    $('base').value=d.base||'';
    if(d.mode==='svr'){
      $('wifiCard').style.display='none';
      $('head').textContent='改服务地址';
      $('sub').textContent='服务连不上：'+(d.base||'');
      needToken=true;
      $('tokLabel').style.display='block';
      $('token').style.display='block';
      showMsg('msg2','看板连不上电脑上的服务。改下面的地址，保存即可生效。\n需要屏幕上那 4 位验证码。',false);
    }else{
      $('sub').textContent='给设备配置 WiFi 与服务地址';
      scan();
    }
  }catch(e){$('sub').textContent='配置';scan();}
})();
</script></body></html>)HTML";

static esp_err_t h_index(httpd_req_t *req)
{
    prov_keepalive();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, k_config_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    prov_keepalive();

    /* ⚠ 只在配网模式下扫描。
     *
     * 配置页现在有两种打开方式，扫描只对其中一种有意义：
     *   · 配网模式（APSTA）：本来就断着 WiFi，扫一下无所谓，页面要列网络名
     *   · "已连 WiFi 改服务地址"：WiFi 是**正常连着**的，此时
     *     esp_wifi_scan_start 会打断现有连接 —— 用户只是想改个地址，
     *     一进页面（h_index 的 JS 会自动 scan()）就把 WiFi 搞断，
     *     屏幕立刻弹 WiFi 浮层、数据也停，然后重连、提示闪烁。
     *
     * svr 模式下页面已经把 WiFi 那张卡片隐藏了，本来也不显示扫描结果，
     * 所以直接回空数组即可，调用方不需要区分"没扫到"和"不让扫"。 */
    if (s_state != WIFI_ST_PROVISION) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "[]", 2);
    }

    /* 配网时模式是 APSTA，可以边当热点边扫描 */
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* true = 阻塞到扫完 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);

    /* ⚠ num 不能截断。
     * esp_wifi_scan_get_ap_records() 要求传入的 number >= 实际扫到的条数，
     * 小了它直接返回错误。原来的写法 "if (num > 20) num = 20;" 把真实条数
     * 改小，于是写字楼/公寓里 AP 一多（>20 很正常）就必然失败，
     * 页面显示"没扫到网络"。要限制的只是**渲染条数**，不是取数条数。 */
    if (!num) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        free(recs);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    /* 只限制输出条数：页面最多列 20 个，其余的丢掉。 */
    if (got > 20) got = 20;

    /* 手拼 JSON。只输出 ssid/rssi/auth 三个字段，页面够用。
     * 每条最多 ~160 字节，开头留 1 字节给 '['，末尾留 3 字节给 "]\0"。 */
    const size_t cap = 64 + (size_t)got * 160;
    char *buf = (char *)malloc(cap);
    if (!buf) { free(recs); return ESP_FAIL; }
    size_t o = 0;
    buf[o++] = '[';
    for (uint16_t i = 0; i < got; i++) {
        if (!recs[i].ssid[0]) continue;              /* 跳过隐藏网络 */

        /* 空间检查必须放在写之前。放后面的话，最后一次 snprintf 可能已经
         * 越界写坏了堆才被 break 拦住 —— 这种 bug 平时不出现，AP 一多就炸。 */
        if (o + 200 + 3 > cap) break;

        char esc[100];
        json_escape(esc, sizeof(esc), (const char *)recs[i].ssid);
        int n = snprintf(buf + o, cap - o,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                         (o > 1 ? "," : ""), esc, recs[i].rssi,
                         recs[i].authmode != WIFI_AUTH_OPEN);
        if (n < 0) break;
        o += (size_t)n;
    }
    buf[o++] = ']';
    buf[o] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, buf, o);
    free(buf);
    free(recs);
    return r;
}

/* esp_http_server 只提供 send_400/send_500 里的 500，400 要自己拼状态行 */
static esp_err_t resp_400(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

/* 把请求体**完整**读进 buf。
 *
 * ⚠ httpd_req_recv() 单次调用不保证读完 Content-Length —— 它返回的是
 * "本次读到的字节数"，可能小于还剩的长度。原代码只调一次：
 *     body[320] 拿到半个 URL 编码串 → form_get 解出乱码 →
 *     用户看到"保存成功"但重启后连不上（重启才失败，现场已丢）。
 *
 * 中文 SSID + 长密码正好是最容易触发的情况（编码后字节数翻倍）。
 *
 * 返回实际长度；读满 buf（放不下）返回 -1，让调用方明确拒绝，
 * 而不是拿截断的内容当完整表单处理。 */
static int req_read_all(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0) return 0;
    if ((size_t)req->content_len >= cap) return -1;   /* 放不下，拒掉 */

    size_t got = 0;
    while (got < (size_t)req->content_len) {
        int n = httpd_req_recv(req, buf + got, (size_t)req->content_len - got);
        if (n <= 0) break;                              /* 对端断了 */
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (int)got;
}

static esp_err_t h_save(httpd_req_t *req)
{
    prov_keepalive();

    /* ⚠ 改 WiFi 凭据只在配网模式下允许。
     * "已连 WiFi"模式（服务连不上时自动开的那个页面）根本不需要重设凭据，
     * 而 /save 会把凭据写进 NVS 并重启设备 —— 无鉴权接口能被别人
     * 用来把设备的 WiFi 改成他自己的。直接拒掉。 */
    if (s_state != WIFI_ST_PROVISION) {
        ESP_LOGW(TAG, "非配网模式拒绝保存 WiFi 凭据");
        return resp_400(req, "provision mode only");
    }

    char body[320] = {};
    int len = req_read_all(req, body, sizeof(body));
    if (len < 0) return resp_400(req, "body too large");
    if (len <= 0) return resp_400(req, "no body");

    /* 局部变量而非静态 buffer：HTTP handler 可能并发进入（见上面的注释），
     * 静态 buffer 会被另一个请求覆盖。 */
    char new_ssid[33] = {};
    char new_pass[65] = {};

    if (!form_get(body, "ssid", new_ssid, sizeof(new_ssid)) || !new_ssid[0]) {
        return resp_400(req, "ssid required");
    }
    if (!form_get(body, "pass", new_pass, sizeof(new_pass))) {
        new_pass[0] = '\0';
    }

    ESP_LOGI(TAG, "配网提交: ssid=%s pass=%s", new_ssid,
             new_pass[0] ? "(已填)" : "(空)");

    /* 先用新凭据试着连一下再存？不这么做。
     * 密码错的话这里要等好几秒，手机那边已经超时了。
     * 直接存 + 重启，连不上用户会再进一次配网页 —— 那时屏幕上会提示失败。 */
    esp_err_t err = creds_save(new_ssid, new_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存失败: %s", esp_err_to_name(err));
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    esp_timer_start_once(s_reboot_timer, REBOOT_DELAY_MS * 1000);
    return ESP_OK;
}

/* 页面用它来决定显示成什么样（配网 / 改服务地址），以及填什么初值 */
static esp_err_t h_info(httpd_req_t *req)
{
    prov_keepalive();

    /* ⚠ 不在"已连 WiFi"模式下回吐 SSID。
     * /info 是无鉴权的，同网段任何设备都能读。SSID 本身不算秘密，
     * 但它能告诉外人"这家 WiFi 叫什么"，配合常见的默认密码就是一条
     * 完整的入侵路径。配网模式下 SSID 本来就要显示在页面上，照给。 */
    const char *ssid_out = (s_state == WIFI_ST_PROVISION) ? s_ssid : "";

    /* 转义后最长按 2 倍算（每个字符最坏变成 \uXXXX 6 字节，实际远达不到，
     * 留 2 倍是为了让编译器能算出确定上界 —— 否则 -Werror=format-truncation
     * 会把这条 snprintf 判成"可能截断"）。 */
    char esc_base[SVR_BASE_MAX + 8];
    char esc_ssid[48];            /* SSID 最长 32，转义后留足 */
    json_escape(esc_base, sizeof(esc_base), s_svr_base);
    json_escape(esc_ssid, sizeof(esc_ssid), ssid_out);

    /* ⚠ 这里**绝对不下发验证码**。
     *
     * 曾经下发过（"页面要用它填表"），但那等于把锁和钥匙一起挂在门上：
     * /info 是无鉴权的，同网段任何人 curl 一下就能拿到，再拿去
     * POST /setbase?t=<那串数字> 就能改服务地址 —— 验证码机制直接作废。
     *
     * 正确设计：验证码只有一个分发渠道，就是**设备屏幕**。用户看到屏幕
     * 上的 4 位数字，手抄进表单。页面从头到尾不需要知道它，也从来没读过
     * （页面只取 mode / base / down 三个字段）。 */

    char body[320];
    snprintf(body, sizeof(body),
             "{\"mode\":\"%s\",\"base\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"down\":%s,\"fails\":%d}",
             (s_state == WIFI_ST_PROVISION) ? "wifi" : "svr",
             esc_base, esc_ssid, s_ip,
             s_svr_down ? "true" : "false", (int)s_svr_fails);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* 改服务地址。不重启 —— 地址是每轮轮询现读的，下一轮就生效。 */
static esp_err_t h_setbase(httpd_req_t *req)
{
    prov_keepalive();

    /* ⚠ 必须校验验证码。
     * 这个接口跑在用户的真实局域网上，同网段任何设备都能调。
     * 不校验的话，攻击者把服务地址改成自己的机器，屏上显示的
     * "工作状态"就全是伪造的 —— 而看板恰恰是用来显示真实状态的。 */
    if (!cfg_token_ok(req)) {
        ESP_LOGW(TAG, "服务地址提交被拒：验证码不对");
        return resp_400(req, "token required");
    }

    char body[160] = {};
    int len = req_read_all(req, body, sizeof(body));
    if (len < 0) return resp_400(req, "body too large");
    if (len <= 0) return resp_400(req, "no body");

    char base[SVR_BASE_MAX] = {};
    if (!form_get(body, "base", base, sizeof(base)) || !base[0]) {
        return resp_400(req, "base required");
    }

    ESP_LOGI(TAG, "配置页提交服务地址: %s", base);
    if (svr_save(base) != ESP_OK) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 任何其它路径都跳回配网页，手机上输错地址也能落到正确页面 */
static esp_err_t h_catchall(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static void http_start(void)
{
    if (s_httpd) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    /* 默认栈 4KB，扫描 + 拼 JSON 会紧一点，给足余量 */
    cfg.stack_size       = 6144;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败，配网页不可用");
        s_httpd = NULL;
        return;
    }

    /* C++ 下不能用"只填部分字段"的 designated initializer（-Werror 会报
     * missing-field-initializers），统一先零初始化再逐字段赋值。 */
    httpd_uri_t u_index = {};
    u_index.uri     = "/";
    u_index.method  = HTTP_GET;
    u_index.handler = h_index;

    httpd_uri_t u_scan = {};
    u_scan.uri      = "/scan";
    u_scan.method   = HTTP_GET;
    u_scan.handler  = h_scan;

    httpd_uri_t u_save = {};
    u_save.uri      = "/save";
    u_save.method   = HTTP_POST;
    u_save.handler  = h_save;

    httpd_uri_t u_info = {};
    u_info.uri      = "/info";
    u_info.method   = HTTP_GET;
    u_info.handler  = h_info;

    httpd_uri_t u_setbase = {};
    u_setbase.uri      = "/setbase";
    u_setbase.method   = HTTP_POST;
    u_setbase.handler  = h_setbase;

    /* 兜底跳转必须最后注册：esp_http_server 按注册顺序找第一个匹配，
     * 通配那条（u_any）会吃掉所有 GET，放前面会把 /scan /info 全截胡。 */
    httpd_uri_t u_any = {};
    u_any.uri       = "/*";
    u_any.method    = HTTP_GET;
    u_any.handler   = h_catchall;

    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_info);
    httpd_register_uri_handler(s_httpd, &u_setbase);
    httpd_register_uri_handler(s_httpd, &u_any);

    ESP_LOGI(TAG, "配置页已就绪: http://%s/",
             (s_state == WIFI_ST_PROVISION) ? s_ap_ip : s_ip);
}

/* 关掉配置页。只在"已连 WiFi 改服务地址"这条路上用 ——
 * 配网模式的页面是等保存后的重启来关的。 */
static void http_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    ESP_LOGI(TAG, "配置页已关闭");
}

/* ---------------------------------------------------------------- 对外 */

void wifi_mgr_begin(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    /* ⚠ 必须关掉驱动的自动持久化。
     *
     * ESP-IDF 的 WiFi 驱动默认会把 STA 配置写进自己的 NVS（`nvs.net80211`），
     * 下次 esp_wifi_start() 时自动读回来连上。后果很隐蔽：
     * 我们已经把"编译期默认 WiFi"删干净了，但驱动里还留着上一次连过的 SSID，
     * 实测设备在"没有任何凭据"的情况下照样连上了旧网络 —— 等于默认 WiFi 没删掉。
     *
     * 改成 RAM only 后，配置只在本次运行有效，凭据的唯一来源就是本文件
     * 管理的 `wbwifi` 命名空间。开机没读到凭据就一定是干净的配网状态。 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    esp_timer_create_args_t a1 = {};
    a1.callback = retry_timer_cb;
    a1.name     = "wretry";
    esp_timer_create(&a1, &s_retry_timer);

    esp_timer_create_args_t a2 = {};
    a2.callback = reboot_timer_cb;
    a2.name     = "wreboot";
    esp_timer_create(&a2, &s_reboot_timer);

    esp_timer_create_args_t a3 = {};
    a3.callback = prov_timeout_cb;
    a3.name     = "wprov";
    esp_timer_create(&a3, &s_prov_timer);

    /* AP 网关地址：向驱动查真实值，不要硬编码。
     *
     * 原来这里写死 "192.168.4.1"。ESP-IDF 默认 AP 网段确实是 192.168.4.1，
     * 但那是**默认值**，会被 sdkconfig 或分区配置改掉；一旦被改，
     * 屏幕上提示的地址就和实际对不上 —— 用户按提示输地址，连不上，
     * 而日志里一切正常。这种"提示和现实不一致"的 bug 极难排查。
     * esp_netif_get_ip_info 拿的是驱动实际生效的值，不会骗人。 */
    s_ap_ip[0] = '\0';   /* 真正取到之前先清空，见下方 start_provision 里的查询 */

    /* 服务地址：NVS 优先，其次编译期 BRIDGE_BASE（由 main.cpp 传入） */
    svr_load();

    /* 凭据：NVS 优先，其次编译期默认值 */
    char ssid[33] = {};
    char pass[65] = {};
    bool have = creds_load(ssid, sizeof(ssid), pass, sizeof(pass));

    if (!have) {
        /* 一次都没配过 —— 直接进配网，别让用户干等 16 秒重试。
         * 没有编译期兜底是刻意的：固件里烧一套默认 WiFi 既没用又泄密。 */
        ESP_LOGW(TAG, "NVS 里没有任何 WiFi 凭据，直接进入配网模式");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        /* 显式清空 STA 配置，双保险：即使驱动里残留了旧 SSID（早期固件
         * 用 esp_wifi_set_config 连过网，会写进 nvs.net80211），这里也覆盖掉。 */
        wifi_config_t empty = {};
        esp_wifi_set_config(WIFI_IF_STA, &empty);
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_mgr_start_provision();
        return;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 开放网络也要能连 */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "连接 WiFi: %s", s_ssid);
}

wifi_state_t wifi_mgr_state(void)          { return s_state; }
const char  *wifi_mgr_ssid(void)           { return s_ssid; }
const char  *wifi_mgr_ip(void)             { return s_ip; }
const char  *wifi_mgr_ap_ssid(void)        { return s_ap_ssid; }
/* 开放热点没有密码，所以也没有"取密码"的接口 —— 屏幕那一行改成显示配置页地址。 */
int          wifi_mgr_ap_clients(void)     { return s_ap_clients; }
const char  *wifi_mgr_ap_ip(void)          { return s_ap_ip; }

void wifi_mgr_retry(void)
{
    if (s_state == WIFI_ST_PROVISION) return;
    s_fail_count = 0;
    s_state = WIFI_ST_CONNECTING;
    ESP_LOGI(TAG, "手动重试");
    esp_wifi_connect();
}

void wifi_mgr_start_provision(void)
{
    if (s_state == WIFI_ST_PROVISION) return;

    s_suppress_reconnect = true;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();

    /* 热点名带 MAC 后两字节，附近有多台看板时能分清 */
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "WorkBuddy-%02X%02X", mac[4], mac[5]);

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {};
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = (uint8_t)strlen(s_ap_ssid);
    ap.ap.password[0]     = '\0';           /* 开放网络：不设密码 */
    ap.ap.max_connection  = 4;
    ap.ap.authmode        = WIFI_AUTH_OPEN;
    ap.ap.channel         = 1;

    /* APSTA：既是热点，又能扫描周围网络给配网页用 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* ⚠ 顺序有讲究：s_state 必须先置成 PROVISION。
     * prov_keepalive() 开头就 if (s_state != WIFI_ST_PROVISION) return;，
     * 状态没到位时调用它，定时器一次都不会启动 —— 配网空闲超时就彻底失效，
     * 误触进配网后只能物理重启。
     * 顺带让 http_start() 的日志也走 AP 分支（配网期该打热点地址）。 */
    s_state = WIFI_ST_PROVISION;

    /* 取 AP 网关真实地址。esp_wifi_set_mode(APSTA) 之后驱动才分配 IP，
     * 所以这一步必须放在 set_mode 之后。取不到就退回默认 192.168.4.1，
     * 至少不会把地址显示成空串。 */
    esp_netif_ip_info_t ap_info = {};
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ap_info) == ESP_OK &&
        ap_info.ip.addr != 0) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ap_info.ip));
    } else {
        snprintf(s_ap_ip, sizeof(s_ap_ip), "192.168.4.1");
        ESP_LOGW(TAG, "拿不到 AP 网关地址，回退到 %s", s_ap_ip);
    }

    http_start();
    prov_keepalive();          /* 起计时：3 分钟没人访问就自己退出 */

    ESP_LOGW(TAG, "进入配网模式：热点 %s（开放网络，无密码）/ 打开 http://%s/",
             s_ap_ssid, s_ap_ip);
}

bool wifi_mgr_clear_creds(void)
{
    bool ok = creds_erase();
    ESP_LOGW(TAG, "清除 WiFi 凭据: %s", ok ? "成功" : "没有可清的");
    s_ssid[0] = '\0';
    return ok;
}

/* ---------------------------------------------------------------- 服务地址对外 */

void wifi_mgr_set_default_base(const char *base)
{
    svr_normalize(base, s_svr_default, sizeof(s_svr_default));
    /* 只有还没定下生效值时才写：NVS 里配过的话 svr_load() 会覆盖它，
     * 这里提前填是为了 set_default_base 晚于 begin() 调用时也有效果。 */
    if (!s_svr_base[0]) {
        strncpy(s_svr_base, s_svr_default, sizeof(s_svr_base) - 1);
        s_svr_base[sizeof(s_svr_base) - 1] = '\0';
    }
}

const char   *wifi_mgr_svr_base(void)    { return s_svr_base; }
uint32_t      wifi_mgr_svr_version(void) { return s_svr_ver; }
bool          wifi_mgr_svr_down(void)    { return s_svr_down; }
int           wifi_mgr_svr_fails(void)   { return s_svr_fails; }

void wifi_mgr_svr_reset(void)
{
    s_svr_fails = 0;
}

void wifi_mgr_svr_report(bool ok)
{
    /* WiFi 本身不通时不记账：这时候拉不到数据是 WiFi 的锅，
     * 不能因此弹"改服务地址"的提示，否则用户会去改一个没问题的地址。 */
    if (s_state != WIFI_ST_CONNECTED) return;

    if (ok) {
        s_svr_fails = 0;
        if (s_svr_down) {
            s_svr_down = false;
            ESP_LOGW(TAG, "服务已恢复，关闭配置页");
            if (s_svr_http) {
                s_svr_http = false;
                http_stop();
            }
        }
        return;
    }

    if (s_svr_fails < SVR_FAIL_MAX) s_svr_fails = s_svr_fails + 1;
    if (s_svr_fails >= SVR_FAIL_MAX && !s_svr_down) {
        s_svr_down = true;
        ESP_LOGW(TAG, "连续 %d 次拉不到服务（%s），开放配置页",
                 (int)s_svr_fails, s_svr_base);
        wifi_mgr_start_svr_config();
    }
}

const char *wifi_mgr_start_svr_config(void)
{
    /* 没连上 WiFi 就没有人能访问这个页面 —— 那种情况该进的是配网热点，
     * 由 wifi_mgr_start_provision() 负责。 */
    if (s_state != WIFI_ST_CONNECTED) {
        ESP_LOGW(TAG, "WiFi 未连接，配置页无处可开");
        return "";
    }

    if (!s_httpd) http_start();
    s_svr_http = true;

    if (!s_ip[0]) {
        ESP_LOGW(TAG, "还没拿到本机 IP，配置页地址暂时未知");
        return "";
    }
    ESP_LOGW(TAG, "配置页: http://%s/ （改服务地址，改完立即生效）", s_ip);
    return s_ip;
}
