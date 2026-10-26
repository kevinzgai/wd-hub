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

#define NVS_NS          "wbwifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

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

/* 配网页提交上来的凭据。用静态 buffer 避免在 HTTP 任务里散落 malloc。 */
static char s_new_ssid[33];
static char s_new_pass[65];

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

/* 每次 HTTP 请求都推迟超时。真正在配网的人不会被中途踢掉。 */
static void prov_keepalive(void)
{
    if (!s_prov_timer) return;
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
<title>看板配网</title><style>
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
<h1>WorkBuddy 看板</h1>
<p class="sub">给设备配置 WiFi</p>

<div class="card">
  <label>可用的 WiFi</label>
  <button class="ghost" id="scan">重新扫描</button>
  <div id="list"><div class="tip">扫描中…</div></div>
</div>

<div class="card">
  <label>WiFi 名称（SSID）</label>
  <input id="ssid" placeholder="点上面选，或直接输入" autocapitalize="off" autocorrect="off">
  <label style="margin-top:14px">密码</label>
  <input id="pass" type="password" placeholder="开放网络留空">
  <button class="primary" id="save">保存并连接</button>
  <div class="msg" id="msg"></div>
  <p class="tip">保存后设备会自动重启去连新 WiFi。<br>
  连不上时长按板上 BOOT 键 3 秒可再次进入本页面。</p>
</div>

<script>
const $=id=>document.getElementById(id);
function showMsg(t,ok){const m=$('msg');m.textContent=t;m.className='msg '+(ok?'ok':'err');}
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
  if(!ssid){showMsg('请先选一个 WiFi',false);return;}
  $('save').disabled=true;$('save').textContent='保存中…';
  try{
    const r=await fetch('/save',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('pass').value)});
    if(r.ok){showMsg('已保存，设备重启中…请稍候',true);}
    else{showMsg('保存失败，请重试',false);$('save').disabled=false;$('save').textContent='保存并连接';}
  }catch(e){showMsg('保存失败：'+e.message,false);$('save').disabled=false;$('save').textContent='保存并连接';}
};
scan();
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
    if (num > 20) num = 20;

    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(num ? num : 1,
                                                        sizeof(wifi_ap_record_t));
    if (!recs) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }
    uint16_t got = num;
    if (num && esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) got = 0;

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

static esp_err_t h_save(httpd_req_t *req)
{
    prov_keepalive();
    char body[320] = {};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return resp_400(req, "no body");

    if (!form_get(body, "ssid", s_new_ssid, sizeof(s_new_ssid)) || !s_new_ssid[0]) {
        return resp_400(req, "ssid required");
    }
    if (!form_get(body, "pass", s_new_pass, sizeof(s_new_pass))) {
        s_new_pass[0] = '\0';
    }

    ESP_LOGI(TAG, "配网提交: ssid=%s pass=%s", s_new_ssid,
             s_new_pass[0] ? "(已填)" : "(空)");

    /* 先用新凭据试着连一下再存？不这么做。
     * 密码错的话这里要等好几秒，手机那边已经超时了。
     * 直接存 + 重启，连不上用户会再进一次配网页 —— 那时屏幕上会提示失败。 */
    esp_err_t err = creds_save(s_new_ssid, s_new_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存失败: %s", esp_err_to_name(err));
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    esp_timer_start_once(s_reboot_timer, REBOOT_DELAY_MS * 1000);
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

    httpd_uri_t u_any = {};
    u_any.uri       = "/*";
    u_any.method    = HTTP_GET;
    u_any.handler   = h_catchall;

    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_any);

    ESP_LOGI(TAG, "配网页已就绪: http://%s/", s_ap_ip);
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

    snprintf(s_ap_ip, sizeof(s_ap_ip), "192.168.4.1");   /* AP 模式固定网关 */

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

    http_start();
    prov_keepalive();          /* 起计时：3 分钟没人访问就自己退出 */
    s_state = WIFI_ST_PROVISION;

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
