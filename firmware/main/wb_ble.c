// main/wb_ble.c —— NimBLE GATT 外设实现。
//
// 三个必须踩准的点(都是实测踩出来的,不是设计偏好):
//   1) 断连后必须无条件重启广播。原固件在 disconnect 回调里只置标志、等上层恢复,
//      结果设备不再广播,主机侧连报 "Device not found",只能复位设备(需求 B-01)。
//   2) 主机写入的 JSON 帧【不带换行】,只有语音轮询帧带 '\n'。所以不能用"按行切",
//      必须按花括号配平判断一帧结束——否则永远收不到整帧。
//   3) notify 单次长度受 MTU 限制,必须切片。主机写的是 ≤480B 的帧,上行帧多数很短,
//      但语音包(2 帧 ADPCM + base64)会到 560B 左右,在 MTU 512 下必然切 2 片。
//      切片必须成段完成——中间插入别的帧会让主机按 '\n' 拼行时错位,
//      所以 wb_ble_send 全程持锁(见该函数注释)。
#include "wb_ble.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "wb_ble";

// 设备蓝牙名（BLE 广播里显示的 Device Name）。
// ⚠ 改这里必须同步改主机侧 host/wb_app.py 的 DEVICE_NAME —— 主机是按名字
// 扫描的（不按 service uuid），两边不一致就永远扫不到设备。
#define WB_DEVICE_NAME "Workbuddy BOX"
#define WB_RX_BUF      1024      // 512B 帧上限 + 余量,防主机误发长帧时越界
#define WB_NO_CONN     0xFFFF
#define WB_BLE_TASK_STACK 4096
#define WB_BLE_TASK_PRIO  5

// 128 位 UUID 在 NimBLE 里按小端字节序给出:第一个参数是字符串的最后一个字节。
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f,
    0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f);

static const ble_uuid128_t s_chr_uuid = BLE_UUID128_INIT(
    0xa8, 0x26, 0x1b, 0x36, 0x07, 0xea, 0xf5, 0xb7,
    0x88, 0x46, 0xe1, 0x36, 0x3e, 0x48, 0xb5, 0xbe);

static wb_ble_frame_cb_t s_on_frame;
static uint16_t s_conn = WB_NO_CONN;
static uint16_t s_tx_handle;
static uint8_t s_addr_type;
static volatile bool s_hello_pending;
static bool s_inited;
static bool s_low_power;              // 待机档：更慢的连接间隔
static int s_active_retry_left;       // 激活后恢复快参数的剩余重试次数
static int64_t s_active_retry_at_us;  // 下一次重试时间
static SemaphoreHandle_t s_tx_lock;   // 串行化 notify 切片，见 wb_ble_send

// 连接参数单位：interval 1.25ms，supervision timeout 10ms。
// 正常档 30~50ms：审批/按键回传要快。
// 待机档 200~250ms：射频唤醒次数约降到 1/4~1/5；latency 固定 0，
// 主机 2s 一次的 status 帧最多多等 250ms，不会因为省电把"新任务唤醒"拖慢。
#define WB_CONN_ITVL_ACTIVE_MIN 24
#define WB_CONN_ITVL_ACTIVE_MAX 40
#define WB_CONN_ITVL_LOW_MIN    160
#define WB_CONN_ITVL_LOW_MAX    200
#define WB_CONN_TIMEOUT_ACTIVE  400   // 4s
#define WB_CONN_TIMEOUT_LOW     600   // 6s
#define WB_CONN_ITVL_FAST_MAX   80    // <=100ms 视为已恢复正常档

static char s_rx[WB_RX_BUF];
static size_t s_rx_len;

static int gap_event(struct ble_gap_event *event, void *arg);

// 把"当前档位"写给控制器。Windows 主机可能拒绝或取自己的最小值，
// 因此每次实际生效后都在 CONN_UPDATE 事件里打印真实参数，便于验收。
static void apply_conn_params(const char *why)
{
    if (s_conn == WB_NO_CONN) return;

    struct ble_gap_upd_params params = {0};
    params.itvl_min = s_low_power ? WB_CONN_ITVL_LOW_MIN : WB_CONN_ITVL_ACTIVE_MIN;
    params.itvl_max = s_low_power ? WB_CONN_ITVL_LOW_MAX : WB_CONN_ITVL_ACTIVE_MAX;
    params.latency = 0;
    params.supervision_timeout = s_low_power ? WB_CONN_TIMEOUT_LOW : WB_CONN_TIMEOUT_ACTIVE;

    const int rc = ble_gap_update_params(s_conn, &params);
    ESP_LOGI(TAG, "请求连接参数(%s): itvl %u-%u(1.25ms) latency 0 timeout %u(10ms) rc=%d",
             why, (unsigned)params.itvl_min, (unsigned)params.itvl_max,
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
    params.conn_mode = BLE_GAP_CONN_MODE_UND;    // 可连接:主机要连上来写帧
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "广播已启动: %s", WB_DEVICE_NAME);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "广播启动失败: %d", rc);
    }
}

// ---- 收帧:按花括号配平切帧(字符串内的花括号不算) ----
static bool frame_complete(const char *buf, size_t len, size_t *frame_len)
{
    int depth = 0;
    bool in_str = false;
    bool escaped = false;
    size_t last = 0;

    for (size_t i = 0; i < len; i++) {
        const char c = buf[i];
        if (c == '\n') {           // 语音轮询帧带换行,按行切
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
        if (depth > 0) last = i;
    }
    (void)last;
    return false;
}

static void rx_consume(void)
{
    for (;;) {
        size_t frame_len = 0;
        if (!frame_complete(s_rx, s_rx_len, &frame_len)) {
            // 缓冲里没有完整帧。若已满说明这一帧本身就是垃圾:整段丢弃,
            // 保留尾部 1 字节以防正好把下一帧的开头切掉。
            if (s_rx_len >= sizeof(s_rx) - 1) {
                ESP_LOGW(TAG, "接收缓冲已满仍无完整帧,整段丢弃 %u 字节", (unsigned)s_rx_len);
                s_rx_len = 0;
            }
            return;
        }
        if (frame_len > 0 && s_on_frame) s_on_frame(s_rx, frame_len);
        const size_t rest = s_rx_len - (frame_len + (s_rx[frame_len] == '\n' ? 1 : 0));
        if (rest > 0) memmove(s_rx, s_rx + (s_rx_len - rest), rest);
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
            ESP_LOGW(TAG, "单次写入 %u 字节放不下(已缓存 %u),丢弃", (unsigned)len,
                     (unsigned)s_rx_len);
            s_rx_len = 0;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (os_mbuf_copydata(ctxt->om, 0, len, s_rx + s_rx_len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_rx_len += len;
        rx_consume();
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        // 主机只写不读;回一个空值,避免某些栈在发现阶段报错。
        return os_mbuf_append(ctxt->om, "", 0) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
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
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_rx_len = 0;
            ESP_LOGI(TAG, "主机已连接 conn=%u", (unsigned)s_conn);
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn, &desc) == 0) {
                ESP_LOGI(TAG, "初始连接参数: itvl=%u(1.25ms) latency=%u timeout=%u(10ms)",
                         (unsigned)desc.conn_itvl, (unsigned)desc.conn_latency,
                         (unsigned)desc.supervision_timeout);
            }
            /* 连上就套当前档位：待机中重连时直接省电，避免先按主机默认跑一段。 */
            apply_conn_params(s_low_power ? "重连-待机" : "连接-正常");
        } else {
            // 连接失败(主机中途放弃等):立即回到可被发现状态。
            ESP_LOGW(TAG, "连接失败 status=%d,重新广播", event->connect.status);
            s_conn = WB_NO_CONN;
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "断开 reason=%d(0x%x),立即重启广播", event->disconnect.reason,
                 event->disconnect.reason);
        s_conn = WB_NO_CONN;
        s_rx_len = 0;
        // B-01:不依赖任何上层状态机,断连就当场上广播。少了这一句设备就再也连不上。
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "广播结束 reason=%d,重启", event->adv_complete.reason);
        advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商为 %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "连接参数已生效: itvl=%u(1.25ms, 约%ums) latency=%u timeout=%u(10ms)",
                     (unsigned)desc.conn_itvl, (unsigned)(desc.conn_itvl * 5 / 4),
                     (unsigned)desc.conn_latency, (unsigned)desc.supervision_timeout);
        } else {
            ESP_LOGW(TAG, "连接参数更新完成但读不到 desc");
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle && event->subscribe.cur_notify) {
            s_hello_pending = true;   // 让应用侧补发 hello(不能在 BLE 上下文里直接写)
            ESP_LOGI(TAG, "主机已订阅 notify,准备发 hello");
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
    nimble_port_run();       // 正常不会返回
    vTaskSuspend(NULL);
}

esp_err_t wb_ble_start(wb_ble_frame_cb_t on_frame)
{
    if (s_inited) return ESP_ERR_INVALID_STATE;
    s_on_frame = on_frame;

#if CONFIG_BT_CTRL_MODEM_SLEEP
    ESP_LOGI(TAG, "控制器 modem sleep 已启用（sleep mode=%d）", CONFIG_BT_CTRL_SLEEP_MODE_EFF);
#else
    ESP_LOGW(TAG, "控制器 modem sleep 未启用");
#endif

    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        ESP_LOGE(TAG, "发送锁创建失败");
        return ESP_ERR_NO_MEM;
    }

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
    // 必须单独开任务跑宿主循环:直接在这里调 nimble_port_run() 会把 app_main 占死,
    // 后面的界面与应用任务就再也起不来了。
    // 栈放 PSRAM：NimBLE 宿主循环深度浅，对栈延迟不敏感，
    // 而内部 RAM 开蓝牙后极度紧张（看板的 8192 栈任务会创建失败）。
    if (xTaskCreateWithCaps(host_task, "wb_nimble", WB_BLE_TASK_STACK, NULL,
                            WB_BLE_TASK_PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "NimBLE 宿主任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wb_ble_connected(void)
{
    return s_conn != WB_NO_CONN;
}

void wb_ble_set_power_mode(bool low_power)
{
    if (s_low_power == low_power) return;
    s_low_power = low_power;
    if (low_power) {
        s_active_retry_left = 0;
    } else {
        /* Windows 有时会把外设请求的"变快"忽略掉（实测待机 247ms 的
         * 请求被接受，但激活后的 30~50ms 请求不生效）。这里留 3 次
         * 1s 间隔重试，等主机下一次 LL 窗口再谈。 */
        s_active_retry_left = 3;
        s_active_retry_at_us = esp_timer_get_time() + 1000000;
    }
    apply_conn_params(low_power ? "待机" : "激活");
}

void wb_ble_poll(void)
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
    ESP_LOGW(TAG, "激活后连接间隔仍为 %u(1.25ms)，重试 %d 次",
             (unsigned)desc.conn_itvl, s_active_retry_left);
    apply_conn_params("激活-重试");
}

bool wb_ble_take_hello(void)
{
    if (!s_hello_pending) return false;
    s_hello_pending = false;
    return true;
}

bool wb_ble_send(const char *data, size_t len)
{
    if (!data || len == 0 || s_conn == WB_NO_CONN) return false;

    const uint16_t mtu = ble_att_mtu(s_conn);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;
    if (chunk == 0) chunk = 20;

    // 一帧必须连续发完。console 任务（ACK / key / battery）与 voice 任务
    // （500B+ 的语音包，MTU 512 时要切 2 片）会并发调用这里：若在切片之间
    // 插进一条别的帧，主机侧按 '\n' 拼行就会把半截 JSON 和 ACK 粘在一起，
    // 两帧一起作废。语音包最容易踩，因为它的切片窗口横跨整个 while 循环。
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
