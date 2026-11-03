// passport-fw/main/wb/wb_link.c —— NimBLE GATT 外设。
//
// 协议 UUID / 设备名与 S3 小智盒子（firmware/main/wb_ble.c）一致，主机
// host/wb_app.py 无需任何改动。实现是本工程独立的一份，不链接 S3 源码。
//
// 三个必须踩准的点（与 S3 同源，均为实测结论）：
//   1) 断连/广播结束后必须无条件重启广播，否则设备再也扫不到；
//   2) 主机写入的 JSON 帧不带换行，必须按花括号配平切帧；
//   3) notify 要按 MTU 切片，且一帧切片期间不能插入别的帧。
#include "wb_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "wb_link";

// ⚠ 必须与 host/wb_app.py 的 DEVICE_NAME 一致：主机按名字扫描，不看 UUID。
#define WB_DEVICE_NAME "Workbuddy BOX"
#define WB_RX_BUF      1024
#define WB_NO_CONN     0xFFFF
#define WB_HOST_STACK  4096
#define WB_HOST_PRIO   5

// NimBLE 的 128 位 UUID 按小端字节序初始化（第一个参数是字符串最后一字节）。
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f,
    0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f);

static const ble_uuid128_t s_chr_uuid = BLE_UUID128_INIT(
    0xa8, 0x26, 0x1b, 0x36, 0x07, 0xea, 0xf5, 0xb7,
    0x88, 0x46, 0xe1, 0x36, 0x3e, 0x48, 0xb5, 0xbe);

static wb_link_frame_cb_t s_on_frame;
static uint16_t s_conn = WB_NO_CONN;
static uint16_t s_tx_handle;
static uint8_t s_addr_type;
static bool s_inited;
static volatile bool s_hello_pending;
static volatile bool s_conn_changed;
static bool s_low_power;

static int s_active_retry_left;
static int64_t s_active_retry_at_us;
static SemaphoreHandle_t s_tx_lock;

// 连接参数单位：interval 1.25ms，supervision timeout 10ms。
// 正常档 30–50ms；待机档 200–250ms（latency 固定 0，不拖慢主机 2s 状态帧）。
#define WB_CONN_ITVL_ACTIVE_MIN 24
#define WB_CONN_ITVL_ACTIVE_MAX 40
#define WB_CONN_ITVL_LOW_MIN    160
#define WB_CONN_ITVL_LOW_MAX    200
#define WB_CONN_TIMEOUT_ACTIVE  400
#define WB_CONN_TIMEOUT_LOW     600
#define WB_CONN_ITVL_FAST_MAX   80

static char s_rx[WB_RX_BUF];
static size_t s_rx_len;

static int gap_event(struct ble_gap_event *event, void *arg);

static void apply_conn_params(const char *why)
{
    if (s_conn == WB_NO_CONN) return;

    struct ble_gap_upd_params params = {0};
    params.itvl_min = s_low_power ? WB_CONN_ITVL_LOW_MIN : WB_CONN_ITVL_ACTIVE_MIN;
    params.itvl_max = s_low_power ? WB_CONN_ITVL_LOW_MAX : WB_CONN_ITVL_ACTIVE_MAX;
    params.latency = 0;
    params.supervision_timeout = s_low_power ? WB_CONN_TIMEOUT_LOW : WB_CONN_TIMEOUT_ACTIVE;

    const int rc = ble_gap_update_params(s_conn, &params);
    ESP_LOGI(TAG, "请求连接参数(%s): itvl %u-%u latency 0 timeout %u rc=%d", why,
             (unsigned)params.itvl_min, (unsigned)params.itvl_max,
             (unsigned)params.supervision_timeout, rc);
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)WB_DEVICE_NAME;
    fields.name_len = (uint8_t)strlen(WB_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播字段设置失败: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "广播已启动: %s", WB_DEVICE_NAME);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "广播启动失败: %d", rc);
    }
}

// 按花括号配平切帧（字符串内的花括号不算）；语音轮询帧以 '\n' 结尾。
static bool frame_complete(const char *buf, size_t len, size_t *frame_len)
{
    int depth = 0;
    bool in_str = false;
    bool escaped = false;

    for (size_t i = 0; i < len; i++) {
        const char c = buf[i];
        if (c == '\n') {
            *frame_len = i;
            return true;
        }
        if (in_str) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                *frame_len = i + 1;
                return true;
            }
        }
    }
    return false;
}

static void rx_consume(void)
{
    for (;;) {
        size_t frame_len = 0;
        if (!frame_complete(s_rx, s_rx_len, &frame_len)) {
            if (s_rx_len >= sizeof(s_rx) - 1) {
                ESP_LOGW(TAG, "接收缓冲已满仍无完整帧，丢弃 %u 字节", (unsigned)s_rx_len);
                s_rx_len = 0;
            }
            return;
        }
        if (frame_len > 0 && s_on_frame) s_on_frame(s_rx, frame_len);
        size_t consumed = frame_len;
        // 帧尾的 '\n' 只在确实位于缓冲区内时才算，避免越界读到脏字节。
        if (consumed < s_rx_len && s_rx[consumed] == '\n') consumed++;
        const size_t rest = s_rx_len - consumed;
        if (rest > 0) memmove(s_rx, s_rx + consumed, rest);
        s_rx_len = rest;
        if (s_rx_len == 0) return;
    }
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0) return 0;
        if (s_rx_len + len > sizeof(s_rx)) {
            ESP_LOGW(TAG, "单次写入 %u 字节放不下（已缓存 %u），丢弃", (unsigned)len,
                     (unsigned)s_rx_len);
            s_rx_len = 0;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (os_mbuf_copydata(ctxt->om, 0, len, s_rx + s_rx_len) != 0)
            return BLE_ATT_ERR_UNLIKELY;
        s_rx_len += len;
        rx_consume();
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return os_mbuf_append(ctxt->om, "", 0) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_chr_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_handle,
    },
    {0},
};

static struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_conn_changed = true;
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_rx_len = 0;
            ESP_LOGI(TAG, "主机已连接 conn=%u", (unsigned)s_conn);
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn, &desc) == 0) {
                ESP_LOGI(TAG, "初始连接参数: itvl=%u latency=%u timeout=%u",
                         (unsigned)desc.conn_itvl, (unsigned)desc.conn_latency,
                         (unsigned)desc.supervision_timeout);
            }
            apply_conn_params(s_low_power ? "重连-待机" : "连接-正常");
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d，重新广播", event->connect.status);
            s_conn = WB_NO_CONN;
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "断开 reason=%d(0x%x)，立即重启广播", event->disconnect.reason,
                 event->disconnect.reason);
        s_conn = WB_NO_CONN;
        s_conn_changed = true;
        s_rx_len = 0;
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "广播结束 reason=%d，重启", event->adv_complete.reason);
        advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商为 %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "连接参数生效: itvl=%u(约%ums) latency=%u timeout=%u",
                     (unsigned)desc.conn_itvl, (unsigned)(desc.conn_itvl * 5 / 4),
                     (unsigned)desc.conn_latency, (unsigned)desc.supervision_timeout);
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle && event->subscribe.cur_notify) {
            s_hello_pending = true;
            ESP_LOGI(TAG, "主机已订阅 notify，等待发 hello");
        }
        return 0;

    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 复位 reason=%d", reason);
    s_conn = WB_NO_CONN;
    s_conn_changed = true;
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "拿不到 BLE 地址");
        return;
    }
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    vTaskSuspend(NULL);
}

esp_err_t wb_link_start(wb_link_frame_cb_t on_frame)
{
    if (s_inited) return ESP_ERR_INVALID_STATE;
    s_on_frame = on_frame;

#if CONFIG_BT_CTRL_MODEM_SLEEP
    ESP_LOGI(TAG, "控制器 modem sleep 已启用（sleep mode=%d）", CONFIG_BT_CTRL_SLEEP_MODE_EFF);
#else
    ESP_LOGW(TAG, "控制器 modem sleep 未启用");
#endif

    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) return ESP_ERR_NO_MEM;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 服务注册失败: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_name_set(WB_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "设备名设置失败: %d", rc);
        return ESP_FAIL;
    }

    s_inited = true;
    // C3 无 PSRAM：宿主循环任务栈必须放内部 RAM。4096 与 S3 版一致。
    if (xTaskCreate(host_task, "wb_nimble", WB_HOST_STACK, NULL, WB_HOST_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "NimBLE 宿主任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wb_link_connected(void) { return s_conn != WB_NO_CONN; }

void wb_link_set_power_mode(bool low_power)
{
    if (s_low_power == low_power) return;
    s_low_power = low_power;
    if (low_power) {
        s_active_retry_left = 0;
    } else {
        // Windows 主机可能忽略“变快”请求，3 次 1s 间隔重试。
        s_active_retry_left = 3;
        s_active_retry_at_us = esp_timer_get_time() + 1000000;
    }
    apply_conn_params(low_power ? "待机" : "激活");
}

void wb_link_poll(void)
{
    if (s_conn == WB_NO_CONN || s_low_power || s_active_retry_left <= 0) return;
    if (esp_timer_get_time() < s_active_retry_at_us) return;

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) != 0) return;
    if (desc.conn_itvl <= WB_CONN_ITVL_FAST_MAX) {
        s_active_retry_left = 0;
        return;
    }

    s_active_retry_left--;
    s_active_retry_at_us = esp_timer_get_time() + 1000000;
    ESP_LOGW(TAG, "激活后连接间隔仍为 %u，重试 %d 次", (unsigned)desc.conn_itvl,
             s_active_retry_left);
    apply_conn_params("激活-重试");
}

bool wb_link_take_hello(void)
{
    if (!s_hello_pending) return false;
    s_hello_pending = false;
    return true;
}

bool wb_link_take_conn_change(bool *connected)
{
    if (!s_conn_changed) return false;
    s_conn_changed = false;
    if (connected) *connected = (s_conn != WB_NO_CONN);
    return true;
}

bool wb_link_send(const char *data, size_t len)
{
    if (!data || len == 0 || s_conn == WB_NO_CONN) return false;

    const uint16_t mtu = ble_att_mtu(s_conn);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    if (s_tx_lock && xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "发送锁超时，丢弃本帧（%uB）", (unsigned)len);
        return false;
    }

    bool ok = true;
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > chunk) n = chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + sent, (uint16_t)n);
        if (!om) {
            ESP_LOGW(TAG, "notify 申请 mbuf 失败");
            ok = false;
            break;
        }
        const int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify 失败: %d", rc);
            ok = false;
            break;
        }
        sent += n;
    }

    if (s_tx_lock) xSemaphoreGive(s_tx_lock);
    return ok;
}
