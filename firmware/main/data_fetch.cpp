/* 看板取数任务 —— 实现（从 main.cpp 搬出）
 *
 * 两条硬约束，改这里之前先读：
 *   1. 单个响应体不能超过 HTTP_BUF_SIZE（4096）。超了会被静默截断 →
 *      JSON 不完整 → 解析出脏数据，不会报错。
 *   2. 失败时**沿用上一份数据**（只把 ok 置 false），别清零 ——
 *      屏幕会闪成一片 0，比显示旧数据更糟。
 */
#include "data_fetch.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"

#include "audio.h"
#include "board_data.h"
#include "board_json.h"
#include "device_report.h"
#include "power_save.h"
#include "wifi_mgr.h"

/* 沿用拆分前的 TAG：主日志格式不变，拆分前后可以直接对比串口输出。 */
static const char *TAG = "wb-board";

/* 服务地址只给 base：第 0 屏走 /status，其余屏走 /screen/<i>。 */
#define BRIDGE_PATH_MAIN  "/status"
/* 轮询周期不写死 —— 深度省电要拉到 60 秒。见 power_save.h。 */
#define HTTP_BUF_SIZE     4096

static char s_http_buf[HTTP_BUF_SIZE];

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} accum_t;

/* 任务句柄。只在本模块用：外部要"立刻重拉"走 data_fetch_kick()。 */
static TaskHandle_t s_fetch_task = NULL;

/* 屏号 / 按键名由 UI 侧提供，每轮现问，不缓存。 */
static int         (*s_current_page)(void) = NULL;
static const char *(*s_last_btn)(void)     = NULL;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    accum_t *a = (accum_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && a) {
        size_t space = a->cap - a->len - 1;
        size_t copy  = (evt->data_len < (int)space) ? (size_t)evt->data_len : space;
        if (copy) {
            memcpy(a->buf + a->len, evt->data, copy);
            a->len += copy;
            a->buf[a->len] = '\0';
        }
    }
    return ESP_OK;
}

static void fetch_task(void *arg)
{
    // 让按键能 xTaskNotifyGive 叫醒本任务，切屏后立刻重拉，不等 5 秒周期
    s_fetch_task = xTaskGetCurrentTaskHandle();

    accum_t acc = { s_http_buf, 0, HTTP_BUF_SIZE };

    esp_http_client_config_t cfg = {};
    cfg.url        = wifi_mgr_svr_base();   /* 初值；每轮循环都会按当前地址重设 */
    cfg.method     = HTTP_METHOD_GET;
    cfg.event_handler = http_event_cb;
    cfg.user_data  = &acc;
    /* 5 秒 -> 8 秒。省电模式下 WiFi 是 MIN_MODEM，下行包要等 beacon
     * 间隔才收（约 100ms 量级），加上服务端偶尔聚合慢一点，
     * 5 秒在 DEEP 档会踩线。放宽到 8 秒，失败率明显下降。
     * 代价：真断网时每轮多等 3 秒 —— 但那种情况本来就在退避（15 秒）。 */
    cfg.timeout_ms = 8000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP 初始化失败");
        vTaskDelete(NULL);
        return;
    }

    int fails = 0;
    uint32_t base_ver = wifi_mgr_svr_version();

    while (1) {
        // WiFi 不通就别拉了。这时 HTTP 只会超时（一次 5 秒）并且把失败计数
        // 算到服务端头上 —— 屏幕会误弹"改服务地址"。等连上再说。
        if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
            wifi_mgr_svr_reset();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }

        // 板载指标上报（温度/电量），30 秒一次。
        // 放在 GET 之前：万一 POST 卡住超时（4 秒），也只影响这一轮的上报，
        // 不会让看板数据晚到 —— 下一轮 GET 照常。
        {
            int64_t now_ms = esp_timer_get_time() / 1000;
            static int64_t last_report_ms = 0;
            if (now_ms - last_report_ms >= DEVICE_REPORT_INTERVAL_MS) {
                last_report_ms = now_ms;
                /* 上报也是个 HTTP POST，DEEP 档下一样会被 MAX_MODEM 卡住。
                 * 好消息是上报失败无所谓（见 device_report.cpp 的注释），
                 * 但白白唤醒一次射频更亏 —— 干脆同样处理。 */
                power_save_http_begin();
                device_report_send();
                power_save_http_end();
            }
        }

        // 每轮都重新读屏号和**服务地址**：按钮随时可能切屏，配置页也随时
        // 可能改地址 —— 两者都不需要重启固件，下一轮就生效。
        int req_page = s_current_page ? s_current_page() : 0;
        const char *base = wifi_mgr_svr_base();
        char url[96];
        if (req_page == 0) {
            snprintf(url, sizeof(url), "%s%s", base, BRIDGE_PATH_MAIN);
        } else {
            snprintf(url, sizeof(url), "%s/screen/%d", base, req_page);
        }
        esp_http_client_set_url(client, url);

        acc.len = 0;
        s_http_buf[0] = '\0';

        /* 深度省电时把射频叫醒，拉完再放回去。
         *
         * 不叫的话 DEEP 档（WIFI_PS_MAX_MODEM + 60 秒轮询）第一轮就
         * ESP_ERR_HTTP_FETCH_HEADER —— 发完请求射频继续睡，响应头
         * 回来时还没醒，5 秒超时先到了。实测必现。
         *
         * 唤醒窗口只覆盖这一次 perform（几十~几百 ms），不影响省电。 */
        power_save_http_begin();

        /* ⚠ DEEP 档必须断开 keep-alive 连接。
         *
         * 服务端 uvicorn 的 timeout_keep_alive=30 秒（见 api_server.py 里
         * 那段注释，当初是给"5 秒轮询"配的 6 倍余量）。DEEP 档 60 秒才
         * 请求一次，**远超 30 秒**，连接早被服务端关掉了，但
         * esp_http_client 的 handle 还攥着那条死连接继续复用 ——
         * 结果就是 ESP_ERR_HTTP_FETCH_HEADER（响应头收不到）。
         *
         * 实测：5 秒轮询时 100% 成功；切到 60 秒后第一轮就失败，
         * 失败点精确落在"距上次成功 60 秒"。
         *
         * 修法：非 ACTIVE 档每次请求前清掉连接。60 秒一次的频率下
         * 复用连接本来也没价值，重建 TCP 的代价（几十 ms）完全可接受。 */
        if (power_save_poll_ms() > 10000) {
            esp_http_client_close(client);
        }

        esp_err_t err = esp_http_client_perform(client);
        power_save_http_end();
        // perform 失败时 handle 里的 status_code 是**上一轮的残留值**，
        // 直接打印会看到 "err=ESP_ERR_HTTP_CONNECT status=200" 这种自相矛盾的日志，
        // 排查时极易误判成"服务端返回 200 但解析失败"。连接没通就没有 HTTP 状态码。
        int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;

        board_data_t parsed = {};
        parsed.page = req_page;                 // 标记这批数据属于哪一屏
        bool got = (err == ESP_OK && status == 200 && acc.len > 0);

        // 服务地址对不对是 wifi_mgr 管的账：连续拉不到就会开放配置页 +
        // 让屏幕弹提示；拉通一次就把提示收回去、页面关掉。
        wifi_mgr_svr_report(got);

        if (got) {
            board_parse(&parsed, s_http_buf);
            parsed.page = req_page;             // board_parse 里 memset 过，要重设
            fails = 0;

            // 首次拉取成功响一声"已连接"。
            // 用静态标志而不是比较快照里的 ok：后者在短暂失败时会沿用旧数据
            // 保持 true，网络抖一下就重响，很烦。这里严格只在**第一次**
            // 拿到 200 时响一次，之后无论断连多少次都不再响。
            static bool s_greeted = false;
            if (!s_greeted) {
                s_greeted = true;
                audio_play(AUDIO_EV_CONNECTED);
            }

            // 屏数由服务端说了算，改屏数不用重烧固件
            board_data_set_nscr(parsed.nscr);
            ESP_LOGI(TAG, "OK p=%d n=%d working=%d done=%d auto=%d sess=%d rows=%d btn=%s",
                     req_page, board_data_nscr(), parsed.working, parsed.today_done,
                     parsed.automations, parsed.session_count, parsed.row_count,
                     s_last_btn ? s_last_btn() : "none");
            // 第 0 屏额外把第 0 条会话的标题打出来。
            // 光看 sess=1 不够 —— 上一版就是"sess=1 但屏上显示空闲中"，
            // 只打计数完全看不出异常。标题是最终要上屏的文字，打它才可核。
            // 只在变化时打，否则 5 秒一条把日志刷满。
            if (req_page == 0) {
                static char last_row0[48] = "\x01";   // 不可能等于真实标题，保证首轮必打
                static uint8_t s_last_st0 = 0xFF;     // 同样不可能等于真实值
                static char     s_last_act0[16] = "\x01";
                const char *r0 = (parsed.session_count > 0)
                                 ? parsed.sessions[0].title : "（空闲中）";
                if (strcmp(r0, last_row0) != 0) {
                    strncpy(last_row0, r0, sizeof(last_row0) - 1);
                    last_row0[sizeof(last_row0) - 1] = '\0';
                    ESP_LOGI(TAG, "   行0 -> %s", r0);
                }

                // 实时状态（st/act）也要打出来。
                // 标题对了不代表状态对了 —— 这两个键是**可选的**，
                // 服务端不发时固件会退化成 st=0，光看标题发现不了
                // "服务端已经在报运行中但固件没解析出来"这种问题。
                if (parsed.session_count > 0) {
                    uint8_t st0 = parsed.sessions[0].state;
                    const char *act0 = parsed.sessions[0].action;
                    if (st0 != s_last_st0 || strcmp(act0, s_last_act0) != 0) {
                        s_last_st0 = st0;
                        strncpy(s_last_act0, act0, sizeof(s_last_act0) - 1);
                        s_last_act0[sizeof(s_last_act0) - 1] = '\0';
                        ESP_LOGI(TAG, "   实时 -> st=%u act=%s", st0,
                                 act0[0] ? act0 : "(空)");
                    }
                }
            }
        } else {
            fails++;
            ESP_LOGW(TAG, "拉取失败 err=%s status=%d (连续%d次)",
                     esp_err_to_name(err), status, fails);
            parsed.ok = false;
            // status==0 表示没拿到 HTTP 响应（连不上/超时），这时写 "HTTP 0"
            // 会让人以为是服务端返回了非法状态码，其实压根没连上。
            if (status > 0) snprintf(parsed.error, sizeof(parsed.error), "HTTP %d", status);
            else            snprintf(parsed.error, sizeof(parsed.error), "%.40s",
                                     esp_err_to_name(err));
            parsed.page = req_page;

            // 短暂失败沿用旧数据，只把状态点变红。
            // sample_ms 跟着旧数据一起留下来 —— 计时锚点不变，
            // 断网期间秒针照常走，比冻在最后一格诚实
            board_data_t prev;
            if (board_data_get(&prev) && prev.ok) {
                int keep = parsed.page;
                parsed = prev;
                parsed.page = keep;
            }
            parsed.ok = false;
        }

        board_data_set(&parsed);

        // 用通知代替死等：切屏时 xTaskNotifyGive 会让它立刻醒来换 URL
        //
        // 周期跟着电源级别走（power_save_poll_ms）：深度省电 60 秒，
        // 其余 5 秒。失败多了再退到 15 秒 —— 服务挂了的时候狂刷没意义，
        // 反而让 WiFi 一直醒着。
        uint32_t base_wait = power_save_poll_ms();
        uint32_t wait = (fails > 3 && base_wait < 15000) ? 15000 : base_wait;
        // 服务地址刚被改过：别等这一轮走完，300ms 后立刻按新地址重试，
        // 否则用户改完地址要干等十几秒才看到屏幕提示消失。
        uint32_t ver = wifi_mgr_svr_version();
        if (ver != base_ver) {
            base_ver = ver;
            wait = 300;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
}

void data_fetch_start(int (*current_page)(void), const char *(*last_btn)(void))
{
    s_current_page = current_page;
    s_last_btn     = last_btn;

    /* ⚠ 检查 xTaskCreate 返回值。
     * 失败时任务根本没起来，但设备看起来是正常的（能亮屏、能连 WiFi），
     * 只是数据永远不刷新 —— 静默失效，最难排查。 */
    if (xTaskCreate(fetch_task, "fetch", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "fetch 任务创建失败 —— 屏幕不会更新数据");
    }
}

void data_fetch_kick(void)
{
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
}
