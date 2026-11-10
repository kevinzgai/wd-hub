// passport-fw/main/wb/vokie_svc.c —— Vokie Passport BLE 语音服务实现。
//
// 移植自 vokie-ai-passport-firmware main/vokie_ble.c（MIT），按本工程结构改造：
//   - NimBLE 初始化 / 广播 / GAP 归 wb_link（本模块只认自己连接上的主机）
//   - 按键手势归 app_wb（launcher 按键分发），本模块只收语义化按键
//   - 启动音 / device_diag / ui_status 移植时省略
//   - codec 采集沿用 wb_voice 的「开录接管 / 停录归还」协议（提示音让位）
//
// 协议要点（AI Passport BLE V1）：
//   Control  = JSON（v:1），≤182B，write+notify，主机写 host_ready / host_state
//   Audio    = 信封（magic 0x5041, v1, session, seq, fragIdx, fragCnt, len）+ ADPCM
//   ADPCM 帧 = 20ms/320 样本 → 166B（2B predictor + 1B step + 2B count + 1B rsv + 160B nibbles）
#include "vokie_svc.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"
#include "wb/ble_audio_mbuf.h"
#include "wb/ble_capture_end.h"
#include "wb/ble_tx_retry.h"
#include "wb/wb_audio.h"
#include "wb/wb_link.h"
#include "wb/wb_power.h"
#include "bsp_audio.h"
#include <stdio.h>

static const char *TAG = "vokie_svc";

#define AUDIO_SAMPLES 320
#define ADPCM_BYTES 166
#define MAX_CONTROL 182
#define EMPTY_FINAL_SEQUENCE 0xffffffffUL
#define AUDIO_TX_TIMEOUT_MS 80
#define CONTROL_TX_TIMEOUT_MS 200
#define TX_RETRY_INTERVAL_MS 5
#define AUDIO_TASK_STACK 5120
#define VOICE_MAX_SECONDS 120

// NimBLE 的 128 位 UUID 按小端字节序初始化。
#define SERVICE_BYTES 0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x01,0x00,0x0e,0x7f
#define CONTROL_BYTES 0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x02,0x00,0x0e,0x7f
#define AUDIO_BYTES   0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x03,0x00,0x0e,0x7f
#define INFO_BYTES    0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x04,0x00,0x0e,0x7f

static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(SERVICE_BYTES);
static const ble_uuid128_t s_control_uuid = BLE_UUID128_INIT(CONTROL_BYTES);
static const ble_uuid128_t s_audio_uuid = BLE_UUID128_INIT(AUDIO_BYTES);
static const ble_uuid128_t s_info_uuid = BLE_UUID128_INIT(INFO_BYTES);

static uint16_t s_control_handle;
static uint16_t s_audio_handle;
static uint16_t s_info_handle;

static uint16_t s_conn;                    // 本服务的连接（订阅时认领）
static volatile bool s_control_subscribed;
static volatile bool s_audio_subscribed;
static volatile bool s_host_ready;
static volatile bool s_recording;
static volatile bool s_stop_requested;
static volatile bool s_result_pending;
static volatile bool s_finishing;
static volatile uint32_t s_session;
static volatile uint32_t s_control_seq;
static volatile uint32_t s_button_seq;
static volatile uint32_t s_epoch = 1;      // 连接代际：断连/换连接即递增
static TaskHandle_t s_audio_task;
static bool s_started;
static void (*s_status_cb)(vokie_status_t);

static const uint16_t NO_CONN = BLE_HS_CONN_HANDLE_NONE;

static void set_status(vokie_status_t st)
{
    if (s_status_cb) s_status_cb(st);
}

static bool host_is_ready(void)
{
    return s_conn != NO_CONN && s_host_ready;
}

// ---- 发送原语（带重试；仅音频任务上下文等待） ----

static int notify_bytes_best_effort(uint16_t handle, const uint8_t *data, size_t len)
{
    if (s_conn == NO_CONN) return BLE_HS_ENOTCONN;
    if (len > ble_att_mtu(s_conn) - 3) return BLE_HS_EMSGSIZE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(s_conn, handle, om);
}

static int notify_control(const char *json)
{
    const size_t len = strlen(json);
    if (len == 0 || len > MAX_CONTROL) return BLE_HS_EMSGSIZE;
    const int rc = notify_bytes_best_effort(s_control_handle, (const uint8_t *)json, len);
    if (rc != 0) ESP_LOGW(TAG, "Control notify rc=%d mtu=%u", rc, ble_att_mtu(s_conn));
    return rc;
}

typedef struct {
    uint16_t conn;
    uint16_t handle;
    uint32_t epoch;
    uint32_t session;
    const uint8_t *data;
    size_t len;
    bool require_recording;
    bool audio_pool;   // 音频帧走大池，控制帧走默认池
} tx_context_t;

static bool tx_valid(void *arg)
{
    const tx_context_t *tx = arg;
    return tx->conn != NO_CONN && tx->conn == s_conn &&
           tx->epoch == s_epoch && tx->session == s_session &&
           (!tx->require_recording || s_recording) &&
           (tx->handle == s_audio_handle ? s_audio_subscribed : s_control_subscribed);
}

static int tx_once(void *arg)
{
    tx_context_t *tx = arg;
    if (!tx_valid(arg)) return BLE_HS_ENOTCONN;
    if (tx->len > ble_att_mtu(tx->conn) - 3) return BLE_HS_EMSGSIZE;
    struct os_mbuf *om = tx->audio_pool
        ? ble_audio_mbuf_from_flat(tx->data, tx->len)
        : ble_hs_mbuf_from_flat(tx->data, tx->len);
    if (!om) return BLE_HS_ENOMEM;
    // NimBLE 无论成败都消费 om；重试需重新分配。
    return ble_gatts_notify_custom(tx->conn, tx->handle, om);
}

static bool tx_retryable(int rc)
{
    return rc == BLE_HS_ENOMEM || rc == BLE_HS_EAGAIN || rc == BLE_HS_EBUSY;
}

static uint32_t tx_now_ms(void *arg)
{
    (void)arg;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void tx_wait_ms(void *arg, uint32_t delay_ms)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms) ? pdMS_TO_TICKS(delay_ms) : 1);
}

static int notify_worker(uint16_t handle, const uint8_t *data, size_t len,
                         uint32_t session, uint32_t epoch, bool require_recording,
                         bool audio_pool, uint32_t timeout_ms)
{
    tx_context_t tx = {.conn = s_conn, .handle = handle, .epoch = epoch, .session = session,
                       .data = data, .len = len, .require_recording = require_recording,
                       .audio_pool = audio_pool};
    const ble_tx_retry_ops_t ops = {&tx, tx_valid, tx_once, tx_retryable, tx_now_ms, tx_wait_ms};
    const ble_tx_retry_result_t result = ble_tx_retry_send(&ops, timeout_ms, TX_RETRY_INTERVAL_MS);
    return result.cancelled ? BLE_HS_ENOTCONN : result.code;
}

static int notify_control_worker(const char *json, uint32_t session, uint32_t epoch)
{
    const size_t len = strlen(json);
    if (!len || len > MAX_CONTROL) return BLE_HS_EMSGSIZE;
    return notify_worker(s_control_handle, (const uint8_t *)json, len, session, epoch,
                         false, false, CONTROL_TX_TIMEOUT_MS);
}

// ---- 会话收尾（ptt_up / device_error；失败则断链防止主机挂在一个死会话上） ----

typedef struct {
    uint32_t session;
    uint32_t epoch;
    uint32_t frames;
    uint16_t conn;
    const char *error;
} capture_end_context_t;

static bool capture_end_valid(void *arg)
{
    const capture_end_context_t *end = arg;
    return end->conn != NO_CONN && end->conn == s_conn &&
           end->epoch == s_epoch && end->session == s_session;
}

static int capture_end_notify(void *arg, bool submit)
{
    const capture_end_context_t *end = arg;
    char json[MAX_CONTROL + 1];
    if (submit) {
        snprintf(json, sizeof(json),
                 "{\"v\":1,\"type\":\"ptt_up\",\"sessionId\":%lu,\"seq\":%lu,\"finalSequence\":%lu}",
                 (unsigned long)end->session, (unsigned long)++s_control_seq,
                 end->frames ? (unsigned long)(end->frames - 1) : EMPTY_FINAL_SEQUENCE);
    } else {
        snprintf(json, sizeof(json),
                 "{\"v\":1,\"type\":\"device_error\",\"sessionId\":%lu,\"message\":\"%s\"}",
                 (unsigned long)end->session, end->error);
    }
    return notify_control_worker(json, end->session, end->epoch);
}

static int capture_end_disconnect(void *arg)
{
    const capture_end_context_t *end = arg;
    return ble_gap_terminate(end->conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int finish_capture(uint32_t session, uint32_t epoch, uint32_t frames, const char *error)
{
    capture_end_context_t end = {session, epoch, frames, s_conn, error};
    const ble_capture_end_ops_t ops = {&end, capture_end_valid, capture_end_notify,
                                       capture_end_disconnect};
    if (!capture_end_valid(&end)) return BLE_HS_ENOTCONN;
    s_finishing = true;
    s_result_pending = true;
    s_recording = false;
    s_stop_requested = false;
    const ble_capture_end_result_t result = ble_capture_end(&ops, error == NULL);
    if (capture_end_valid(&end)) s_finishing = false;
    return result.cancelled ? BLE_HS_ENOTCONN : result.notify_rc;
}

// ---- IMA ADPCM（vokie 帧格式：320 样本 → 166B，逐帧独立） ----

static int ima_index_table[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static int ima_step_table[89] = {
 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
 157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,
 1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
 8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};

static size_t encode_adpcm(const int16_t *pcm, uint8_t out[ADPCM_BYTES])
{
    int predictor = pcm[0];
    int index = 0;
    out[0] = (uint8_t)predictor; out[1] = (uint8_t)(predictor >> 8);
    out[2] = 0; out[3] = AUDIO_SAMPLES & 0xff; out[4] = AUDIO_SAMPLES >> 8; out[5] = 0;
    int nibble = 0;
    for (int i = 1; i < AUDIO_SAMPLES; ++i) {
        int diff = pcm[i] - predictor;
        int sign = diff < 0 ? 8 : 0;
        if (diff < 0) diff = -diff;
        int step = ima_step_table[index], delta = 0, vpdiff = step >> 3;
        if (diff >= step) { delta |= 4; diff -= step; vpdiff += step; }
        if (diff >= (step >> 1)) { delta |= 2; diff -= step >> 1; vpdiff += step >> 1; }
        if (diff >= (step >> 2)) { delta |= 1; vpdiff += step >> 2; }
        const int code = delta | sign;
        predictor += sign ? -vpdiff : vpdiff;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        index += ima_index_table[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        if (nibble == 0) { out[6 + (i - 1) / 2] = (uint8_t)code; nibble = 1; }
        else { out[6 + (i - 1) / 2] |= (uint8_t)(code << 4); nibble = 0; }
    }
    return ADPCM_BYTES;
}

// 音频信封：MTU 185 时 166B 载荷恰好一个通知（185-3-16=166）。
static int notify_audio(uint32_t session, uint32_t epoch, uint32_t sequence,
                        const uint8_t *payload, size_t len)
{
    if (s_conn == NO_CONN || epoch != s_epoch || session != s_session) return BLE_HS_ENOTCONN;
    const size_t mtu_payload = ble_att_mtu(s_conn) - 3;
    if (mtu_payload <= 16) return BLE_HS_EMSGSIZE;
    const size_t chunk = mtu_payload - 16;
    uint8_t packet[16 + ADPCM_BYTES];
    const uint8_t count = (uint8_t)((len + chunk - 1) / chunk);
    if (count == 0 || count > 64) return BLE_HS_EMSGSIZE;
    for (uint8_t part = 0; part < count; ++part) {
        const size_t offset = part * chunk;
        const size_t amount = len - offset < chunk ? len - offset : chunk;
        packet[0] = 0x41; packet[1] = 0x50; packet[2] = 1; packet[3] = 0;
        memcpy(packet + 4, &session, 4); memcpy(packet + 8, &sequence, 4);
        packet[12] = part; packet[13] = count;
        packet[14] = amount & 0xff; packet[15] = amount >> 8;
        memcpy(packet + 16, payload + offset, amount);
        const int rc = notify_worker(s_audio_handle, packet, 16 + amount, session, epoch,
                                     true, true, AUDIO_TX_TIMEOUT_MS);
        if (rc != 0) return rc;
    }
    return 0;
}

// ---- codec 接管/归还（与 wb_audio 提示音通道换手，协议同 wb_voice） ----

static bool codec_take_for_capture(void)
{
    wb_audio_deinit();                 // 提示音让位，否则 profile 冲突
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "codec duplex 初始化失败");
        wb_audio_init();
        return false;
    }
    if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "16kHz 采集格式设置失败");
        bsp_audio_deinit();
        wb_audio_init();
        return false;
    }
    return true;
}

static void codec_give_back(void)
{
    bsp_audio_deinit();
    if (!wb_audio_init()) ESP_LOGW(TAG, "提示音通道恢复失败");
}

// ---- 音频任务：一次通知 = 一个会话 ----

static void audio_task(void *arg)
{
    (void)arg;
    int16_t pcm[AUDIO_SAMPLES];
    uint8_t encoded[ADPCM_BYTES];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_recording) continue;

        const uint32_t session = s_session;
        const uint32_t epoch = s_epoch;
        ESP_LOGI(TAG, "Capture start session=%lu epoch=%lu",
                 (unsigned long)session, (unsigned long)epoch);

        if (!codec_take_for_capture()) {
            finish_capture(session, epoch, 0, "audio_open");
            set_status(VOKIE_STS_ERROR);
            continue;
        }

        char start_json[MAX_CONTROL + 1];
        snprintf(start_json, sizeof(start_json),
                 "{\"v\":1,\"type\":\"ptt_down\",\"sessionId\":%lu,\"seq\":0}",
                 (unsigned long)session);
        if (notify_control_worker(start_json, session, epoch) != 0) {
            codec_give_back();
            finish_capture(session, epoch, 0, "start_tx");
            set_status(VOKIE_STS_ERROR);
            continue;
        }
        set_status(VOKIE_STS_RECORDING);

        uint32_t seq = 0;
        bool transport_fail = false;
        int64_t last_activity_us = esp_timer_get_time();
        while (s_recording && session == s_session && epoch == s_epoch) {
            if (s_stop_requested) break;
            if (esp_timer_get_time() - last_activity_us >=
                (int64_t)VOICE_MAX_SECONDS * 1000000LL) {
                ESP_LOGI(TAG, "录音 %ds 到限，自动停", VOICE_MAX_SECONDS);
                s_stop_requested = true;
                break;
            }
            if (!s_audio_subscribed || ble_att_mtu(s_conn) < 185) {
                ESP_LOGW(TAG, "音频链路不可用（订阅/MTU），中止");
                transport_fail = true;
                break;
            }
            if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (!s_recording || session != s_session || epoch != s_epoch) break;

            encode_adpcm(pcm, encoded);
            if (notify_audio(session, epoch, seq, encoded, ADPCM_BYTES) != 0) {
                ESP_LOGE(TAG, "音频发送失败 seq=%lu", (unsigned long)seq);
                transport_fail = true;
                break;
            }
            ++seq;
            last_activity_us = esp_timer_get_time();
            // 每秒喂一次活动：待机/熄屏不进来，BLE 保持快连。
            if (seq % 50 == 0) wb_power_note_activity();
        }

        const uint32_t frames = seq;
        int end_rc;
        if (transport_fail) {
            end_rc = finish_capture(session, epoch, frames, "transport");
            set_status(VOKIE_STS_ERROR);
        } else if (session == s_session && epoch == s_epoch) {
            end_rc = finish_capture(session, epoch, frames, NULL);
            set_status(end_rc == 0 ? VOKIE_STS_PROCESSING : VOKIE_STS_ERROR);
        } else {
            end_rc = BLE_HS_ENOTCONN;
        }
        codec_give_back();
        ESP_LOGI(TAG, "Capture end session=%lu frames=%lu rc=%d",
                 (unsigned long)session, (unsigned long)frames, end_rc);
    }
}

// ---- 按键（Vokie 模式路由进来） ----

static const char *button_name(uint8_t btn)
{
    // BSP_BTN_UP=0 / DOWN=1 / OK=2（bsp_button.h）
    return btn == 1 ? "down" : btn == 2 ? "ok" : "up";
}

static void send_button_event(uint8_t btn, const char *event, uint32_t duration_ms)
{
    if (!host_is_ready()) return;
    char json[MAX_CONTROL + 1];
    snprintf(json, sizeof(json),
             "{\"v\":1,\"type\":\"button_event\",\"button\":\"%s\",\"event\":\"%s\","
             "\"durationMs\":%lu,\"seq\":%lu}",
             button_name(btn), event, (unsigned long)duration_ms,
             (unsigned long)++s_button_seq);
    notify_control(json);
}

static void start_session(void)
{
    if (s_finishing || s_result_pending) {
        set_status(VOKIE_STS_PROCESSING);
        return;
    }
    s_session = esp_random();
    if (s_session == 0) s_session = 1;
    s_control_seq = 0;
    s_stop_requested = false;
    s_recording = true;               // 先置位，任务通知后立刻进采集
    if (s_audio_task) xTaskNotifyGive(s_audio_task);
}

static void stop_session(void)
{
    if (!s_recording) return;
    s_stop_requested = true;
    set_status(VOKIE_STS_PROCESSING);
}

void vokie_svc_key(uint8_t btn, bool long_press)
{
    if (!host_is_ready()) return;
    if (btn == 0 && !long_press) {          // UP click：PTT 开关
        if (!s_recording) start_session();
        else stop_session();
    } else if (btn == 2) {                  // OK
        if (s_recording) {
            stop_session();
            send_button_event(btn, long_press ? "long" : "click", 0);
        } else if (!long_press) {
            send_button_event(btn, "click", 0);   // 空闲删字
        }
        // OK 长按且空闲：留给上层当「退出模式」，不上报 clear_input。
    } else if (btn == 1 && !long_press) {   // DOWN click：回车
        send_button_event(btn, "click", 0);
    }
}

void vokie_svc_cancel(void)
{
    if (s_recording) stop_session();
}

// ---- GAP 事件（wb_link 转发） ----

static void send_hello(void)
{
    const char hello[] = "{\"v\":1,\"type\":\"hello\",\"device\":\"ai-passport\","
                         "\"fw\":\"0.1.0\",\"codec\":\"ima-adpcm\",\"sampleRate\":16000,"
                         "\"channels\":1,\"frameMs\":20}";
    if (notify_control(hello) == 0) ESP_LOGI(TAG, "hello 已发送");
}

static void request_audio_link(void)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = 12, .itvl_max = 12, .latency = 0,
        .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
    };
    // 15ms 固定间隔：宽区间会让 Windows 停在 30ms，20ms 音频节拍会耗尽 TX 缓冲。
    int rc = ble_gap_update_params(s_conn, &params);
    ESP_LOGI(TAG, "音频链路参数请求 rc=%d target=15ms", rc);
    rc = ble_gap_set_data_len(s_conn, 251, 2120);
    ESP_LOGI(TAG, "数据长度扩展 rc=%d tx_octets=251", rc);
}

void vokie_svc_on_gap_event(struct ble_gap_event *event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ++s_epoch;
            ESP_LOGI(TAG, "连接 conn=%u（待订阅认领）", event->connect.conn_handle);
        }
        return;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_conn) {
            ESP_LOGW(TAG, "Vokie 主机断开 reason=%d", event->disconnect.reason);
            ++s_epoch;
            s_conn = NO_CONN;
            s_recording = false;
            s_stop_requested = false;
            s_host_ready = false;
            s_result_pending = false;
            s_finishing = false;
            s_control_subscribed = false;
            s_audio_subscribed = false;
            set_status(VOKIE_STS_DISCONNECTED);
        }
        return;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (s_conn == NO_CONN ||
            event->subscribe.conn_handle == s_conn) {
            if (event->subscribe.attr_handle == s_control_handle) {
                s_control_subscribed = event->subscribe.cur_notify;
                if (event->subscribe.cur_notify) {
                    if (s_conn == NO_CONN) {
                        s_conn = event->subscribe.conn_handle;
                        ESP_LOGI(TAG, "Vokie 主机认领 conn=%u", s_conn);
                    }
                }
            } else if (event->subscribe.attr_handle == s_audio_handle) {
                s_audio_subscribed = event->subscribe.cur_notify;
                if (event->subscribe.cur_notify && s_conn == NO_CONN) {
                    s_conn = event->subscribe.conn_handle;
                    ESP_LOGI(TAG, "Vokie 主机认领 conn=%u", s_conn);
                }
            }
            ESP_LOGI(TAG, "Subscribe handle=%u notify=%u control=%u audio=%u",
                     event->subscribe.attr_handle, event->subscribe.cur_notify,
                     s_control_subscribed, s_audio_subscribed);
            if (s_control_subscribed && s_audio_subscribed &&
                event->subscribe.conn_handle == s_conn) {
                send_hello();
            }
        }
        return;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU conn=%u mtu=%u", event->mtu.conn_handle, event->mtu.value);
        return;

    default:
        return;
    }
}

// ---- GATT 访问 ----

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    const int kind = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && kind == 3) {
        static const char info[] =
            "{\"v\":1,\"type\":\"hello\",\"device\":\"ai-passport\",\"fw\":\"0.1.0\","
            "\"codec\":\"ima-adpcm\",\"sampleRate\":16000,\"channels\":1,\"frameMs\":20}";
        return os_mbuf_append(ctxt->om, info, strlen(info));
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || kind != 1)
        return BLE_ATT_ERR_READ_NOT_PERMITTED;

    char value[MAX_CONTROL + 1] = {0};
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, value, MAX_CONTROL, &len) != 0 ||
        len == 0 || len > MAX_CONTROL)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    cJSON *root = cJSON_ParseWithLength(value, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }
    const cJSON *v = cJSON_GetObjectItem(root, "v");
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsNumber(v) || v->valueint != 1 || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (strcmp(type->valuestring, "host_ready") == 0) {
        if (conn != s_conn || !s_control_subscribed || !s_audio_subscribed ||
            ble_att_mtu(conn) < 185) {
            ESP_LOGW(TAG, "host_ready 拒绝 conn=%u control=%u audio=%u mtu=%u",
                     conn, s_control_subscribed, s_audio_subscribed, ble_att_mtu(conn));
            cJSON_Delete(root);
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_host_ready = true;
        s_result_pending = false;
        ESP_LOGI(TAG, "Host ready epoch=%lu", (unsigned long)s_epoch);
        request_audio_link();
        set_status(VOKIE_STS_READY);
    } else if (strcmp(type->valuestring, "host_state") == 0) {
        const cJSON *state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state)) {
            const char *st = state->valuestring;
            if (strcmp(st, "ready") == 0) {
                s_host_ready = true;
                if (!s_recording) s_result_pending = false;
                set_status(VOKIE_STS_READY);
            } else if (strcmp(st, "recording") == 0) {
                set_status(VOKIE_STS_RECORDING);
            } else if (strcmp(st, "processing") == 0) {
                s_result_pending = true;
                set_status(VOKIE_STS_PROCESSING);
            } else if (strcmp(st, "success") == 0) {
                if (!s_recording) {
                    s_result_pending = false;
                    set_status(VOKIE_STS_SUCCESS);
                }
            } else if (strcmp(st, "error") == 0) {
                s_result_pending = false;
                ESP_LOGW(TAG, "主机报错，取消采集");
                s_recording = false;
                s_stop_requested = false;
                set_status(VOKIE_STS_ERROR);
            }
        }
    }
    cJSON_Delete(root);
    return 0;
}

static const struct ble_gatt_svc_def s_vokie_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &s_service_uuid.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &s_control_uuid.u, .access_cb = access_cb, .arg = (void *)(intptr_t)1,
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
          .val_handle = &s_control_handle},
         {.uuid = &s_audio_uuid.u, .access_cb = access_cb, .arg = (void *)(intptr_t)2,
          .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_audio_handle},
         {.uuid = &s_info_uuid.u, .access_cb = access_cb, .arg = (void *)(intptr_t)3,
          .flags = BLE_GATT_CHR_F_READ, .val_handle = &s_info_handle},
         {0}}},
    {0},
};

// ---- 对外接口 ----

void vokie_svc_register_gatt(void)
{
    if (ble_gatts_count_cfg(s_vokie_svcs) != 0 ||
        ble_gatts_add_svcs(s_vokie_svcs) != 0) {
        ESP_LOGE(TAG, "Vokie GATT 服务注册失败");
    }
}

void vokie_svc_start(void)
{
    if (s_started) return;
    wb_link_set_gap_cb(vokie_svc_on_gap_event);
    if (xTaskCreate(audio_task, "vokie_audio", AUDIO_TASK_STACK, NULL, 5, &s_audio_task) !=
        pdPASS) {
        ESP_LOGE(TAG, "vokie_audio 任务创建失败");
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "Vokie 语音服务就绪");
}

const ble_uuid_t *vokie_svc_service_uuid(void) { return &s_service_uuid.u; }

bool vokie_svc_host_ready(void) { return host_is_ready(); }

bool vokie_svc_is_recording(void) { return s_recording; }

void vokie_svc_set_status_cb(void (*cb)(vokie_status_t st)) { s_status_cb = cb; }
