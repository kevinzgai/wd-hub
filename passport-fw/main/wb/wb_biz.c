// passport-fw/main/wb/wb_biz.c —— 收帧 → 状态机 → ACK / 上行。
//
// 设计边界：
//   * 本文件不调用任何 LVGL API，UI 通过 wb_biz_snapshot() 拉取只读快照；
//   * 所有共享状态由 s_mutex 保护；
//   * BLE 回调只把整帧推进队列，真正的解析/ACK 在 wb_task 里做。
#include "wb_biz.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wb_link.h"

static const char *TAG = "wb_biz";

#define WB_QUEUE_DEPTH 4
#define WB_TASK_STACK  4096
#define WB_TASK_PRIO   4
#define WB_APPROVAL_ID_NONE (-1)

typedef struct {
    char data[WB_RECV_MAX + 1];
    size_t len;
} wb_frame_msg_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;
static wb_biz_change_cb_t s_on_change;
static bool s_inited;

static wb_view_t s_view;
static int s_last_n_sess;
static bool s_last_running[WB_MAX_SESS];
static unsigned s_wake_bits;
static int s_answered_approval_id = WB_APPROVAL_ID_NONE;
static bool s_approval_auto = true;   // 长按 DOWN 返回总览后不再自动弹回审批页

static bool is_running(const wb_sess_t *s)
{
    return s->state[0] != '\0' && strcmp(s->state, "RUNNING") == 0;
}

static void view_lock(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void view_unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

static void notify_change(void)
{
    if (s_on_change) s_on_change();
}

static void recompute_busy_locked(void)
{
    bool busy = false;
    for (int i = 0; i < s_view.n_sess; ++i)
        if (is_running(&s_view.sess[i])) busy = true;
    busy = busy || s_view.approval || s_view.ask;
    s_view.busy = busy;
}

static void apply_status_locked(const wb_ev_t *ev)
{
    unsigned wake = 0;

    /* 新任务：会话条数变多。主机 status 帧的 tasks 是“当前活跃会话”，
     * 新增一条即代表新任务起步，这是最可靠的唤醒信号。 */
    if (ev->n_sess > s_last_n_sess) wake |= WB_WAKE_TASK;

    for (int i = 0; i < ev->n_sess; ++i) {
        const bool running = is_running(&ev->sess[i]);
        if (running && !(i < s_last_n_sess && s_last_running[i])) wake |= WB_WAKE_ACTIVE;
    }

    s_last_n_sess = ev->n_sess;
    for (int i = 0; i < WB_MAX_SESS; ++i)
        s_last_running[i] = i < ev->n_sess && is_running(&ev->sess[i]);

    s_view.have_status = true;
    s_view.n_sess = ev->n_sess;
    memcpy(s_view.sess, ev->sess, sizeof(s_view.sess));
    if (ev->clock_s > 0) s_view.clock_s = ev->clock_s;

    if (s_view.sel >= s_view.n_sess) s_view.sel = s_view.n_sess > 0 ? s_view.n_sess - 1 : 0;
    if (s_view.page == WB_PAGE_APPROVAL && !s_view.approval) s_view.page = WB_PAGE_OVERVIEW;
    if (s_view.page == WB_PAGE_ASK && !s_view.ask) s_view.page = WB_PAGE_OVERVIEW;

    s_wake_bits |= wake;
    recompute_busy_locked();
}

static void apply_approval_locked(const wb_ev_t *ev)
{
    if (ev->approval_id == s_answered_approval_id) return;   // 已作答，忽略重复帧

    s_view.approval = true;
    s_view.approval_id = ev->approval_id;
    snprintf(s_view.tool, sizeof(s_view.tool), "%s", ev->tool);
    snprintf(s_view.hint, sizeof(s_view.hint), "%s", ev->hint);
    s_approval_auto = true;

    s_view.page = s_view.ask ? WB_PAGE_ASK : WB_PAGE_APPROVAL;
    s_view.sel = 0;
    s_wake_bits |= WB_WAKE_APPROVAL;
    recompute_busy_locked();
}

static void apply_ask_locked(const wb_ev_t *ev)
{
    s_view.ask = true;
    s_view.ask_id = ev->ask_id;
    s_view.ask_ans = ev->ask_ans;
    s_view.n_opts = ev->n_opts;
    snprintf(s_view.text, sizeof(s_view.text), "%s", ev->text);
    for (int i = 0; i < ev->n_opts; ++i)
        snprintf(s_view.opt[i], sizeof(s_view.opt[i]), "%s", ev->opt[i]);

    s_view.page = WB_PAGE_ASK;   // 提问优先于审批
    s_view.sel = 0;
    s_wake_bits |= WB_WAKE_ASK;
    recompute_busy_locked();
}

static void apply_ask_end_locked(int id)
{
    if (!s_view.ask || id != s_view.ask_id) return;
    s_view.ask = false;
    s_view.n_opts = 0;
    if (s_view.page == WB_PAGE_ASK) s_view.page = WB_PAGE_OVERVIEW;
    recompute_busy_locked();
}

static void handle_frame(const wb_frame_msg_t *msg)
{
    wb_ev_t ev;
    char ack_scratch[32];
    const char *ack = NULL;
    bool changed = false;

    if (!wb_proto_decode(msg->data, msg->len, &ev)) {
        ESP_LOGW(TAG, "帧解析失败（%uB）：%.80s", (unsigned)msg->len, msg->data);
        wb_link_send("ERR_JSON\n", 9);
        return;
    }

    view_lock();
    switch (ev.type) {
    case WB_EV_STATUS:
        apply_status_locked(&ev);
        changed = true;
        break;
    case WB_EV_APPROVAL:
        apply_approval_locked(&ev);
        changed = true;
        break;
    case WB_EV_ASK:
        apply_ask_locked(&ev);
        changed = true;
        break;
    case WB_EV_ASK_END:
        apply_ask_end_locked(ev.ask_id);
        changed = true;
        break;
    case WB_EV_CREDIT:
    case WB_EV_CFG:
    case WB_EV_MSG:
        changed = true;   // 仅 ACK，不改界面
        break;
    case WB_EV_USAGE:
    case WB_EV_NONE:
    default:
        break;
    }
    view_unlock();

    ack = wb_proto_ack_for(ev.type, ack_scratch, sizeof(ack_scratch), ev.beep_vol);
    if (ack) {
        // 主机按 '\n' 切行解析上行，裸文本 ACK 必须自带换行，
        // 否则会和下一帧粘成一行（S3 固件踩过这个坑）。
        char line[48];
        const int n = snprintf(line, sizeof(line), "%s\n", ack);
        if (n > 0 && (size_t)n < sizeof(line)) wb_link_send(line, (size_t)n);
    }

    if (changed) notify_change();
}

static void wb_task(void *arg)
{
    (void)arg;
    wb_frame_msg_t msg;
    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) handle_frame(&msg);
    }
}

static void on_ble_frame(const char *frame, size_t len)
{
    if (!s_queue || len > WB_RECV_MAX) return;
    wb_frame_msg_t msg;
    msg.len = len;
    memcpy(msg.data, frame, len);
    msg.data[len] = '\0';
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
        ESP_LOGW(TAG, "收帧队列已满，丢弃 %uB 帧", (unsigned)len);
}

bool wb_biz_start(wb_biz_change_cb_t on_change)
{
    if (s_inited) return true;
    s_on_change = on_change;

    memset(&s_view, 0, sizeof(s_view));
    s_view.page = WB_PAGE_OVERVIEW;
    s_answered_approval_id = WB_APPROVAL_ID_NONE;

    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(WB_QUEUE_DEPTH, sizeof(wb_frame_msg_t));
    if (!s_mutex || !s_queue) {
        ESP_LOGE(TAG, "业务层队列/锁创建失败");
        return false;
    }

    if (wb_link_start(on_ble_frame) != ESP_OK) {
        ESP_LOGE(TAG, "BLE 启动失败，业务层继续运行（等待重启）");
    }

    if (xTaskCreate(wb_task, "wb_task", WB_TASK_STACK, NULL, WB_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wb_task 创建失败");
        return false;
    }

    s_inited = true;
    ESP_LOGI(TAG, "业务层已启动");
    return true;
}

void wb_biz_snapshot(wb_view_t *out)
{
    if (!out) return;
    view_lock();
    *out = s_view;
    view_unlock();
}

unsigned wb_biz_take_wake(void)
{
    view_lock();
    unsigned bits = s_wake_bits;
    s_wake_bits = 0;
    view_unlock();
    return bits;
}

void wb_biz_set_connected(bool connected)
{
    if (!s_inited) return;
    view_lock();
    bool changed = s_view.connected != connected;
    s_view.connected = connected;
    view_unlock();
    if (changed) notify_change();
}

void wb_biz_move(int delta)
{
    if (!s_inited || delta == 0) return;
    view_lock();
    if (s_view.page == WB_PAGE_OVERVIEW) {
        if (s_view.n_sess > 0) {
            int sel = s_view.sel + delta;
            if (sel < 0) sel = 0;
            if (sel >= s_view.n_sess) sel = s_view.n_sess - 1;
            s_view.sel = sel;
        }
    } else if (s_view.page == WB_PAGE_APPROVAL) {
        s_view.sel = s_view.sel == 0 ? 1 : 0;
    } else if (s_view.page == WB_PAGE_ASK) {
        if (s_view.n_opts > 0) {
            int sel = s_view.sel + delta;
            if (sel < 0) sel = 0;
            if (sel >= s_view.n_opts) sel = s_view.n_opts - 1;
            s_view.sel = sel;
        }
    }
    view_unlock();
    notify_change();
}

void wb_biz_confirm(void)
{
    if (!s_inited) return;

    char out[128];
    size_t n = 0;
    int id = 0;
    int index = 0;
    const char *key = NULL;
    bool changed = false;

    view_lock();
    if (s_view.page == WB_PAGE_OVERVIEW) {
        // 总览页按 OK：有审批则进审批页，否则有提问进提问页。
        if (s_view.approval && s_approval_auto) {
            s_view.page = WB_PAGE_APPROVAL;
            s_view.sel = 0;
            changed = true;
        } else if (s_view.ask) {
            s_view.page = WB_PAGE_ASK;
            s_view.sel = 0;
            changed = true;
        }
    } else if (s_view.page == WB_PAGE_APPROVAL && s_view.approval) {
        id = s_view.approval_id;
        key = s_view.sel == 0 ? "ok" : "down";
        s_view.approval = false;
        s_answered_approval_id = id;
        s_view.page = WB_PAGE_OVERVIEW;
        recompute_busy_locked();
        changed = true;
    } else if (s_view.page == WB_PAGE_ASK && s_view.ask) {
        if (s_view.ask_ans != 0 && s_view.n_opts > 0) {
            id = s_view.ask_id;
            index = s_view.sel;
            n = wb_proto_enc_ask_index(out, sizeof(out), id, index);
            s_view.ask = false;
            s_view.page = WB_PAGE_OVERVIEW;
            recompute_busy_locked();
            changed = true;
        }
    }
    view_unlock();

    if (key) {
        n = wb_proto_enc_key(out, sizeof(out), id, key);
        ESP_LOGI(TAG, "审批作答 id=%d key=%s", id, key);
    } else if (n > 0) {
        ESP_LOGI(TAG, "提问作答 id=%d index=%d", id, index);
    } else if (!changed) {
        return;
    }

    if (n > 0 && !wb_link_send(out, n)) ESP_LOGW(TAG, "上行发送失败（未连接？）");
    if (changed) notify_change();
}

void wb_biz_back(void)
{
    if (!s_inited) return;

    char out[96];
    size_t n = 0;
    int ask_id = 0;
    bool changed = false;

    view_lock();
    if (s_view.page == WB_PAGE_APPROVAL) {
        s_view.page = WB_PAGE_OVERVIEW;
        s_approval_auto = false;   // 用户主动返回，不自动弹回
        changed = true;
    } else if (s_view.page == WB_PAGE_ASK && s_view.ask) {
        // 主机约定：长按 DOWN 用 key 帧 + approval_id=ask.id + "skip"。
        ask_id = s_view.ask_id;
        n = wb_proto_enc_key(out, sizeof(out), ask_id, "skip");
        s_view.ask = false;
        s_view.page = WB_PAGE_OVERVIEW;
        recompute_busy_locked();
        changed = true;
    }
    view_unlock();

    if (n > 0 && !wb_link_send(out, n)) ESP_LOGW(TAG, "skip 发送失败（未连接？）");
    if (changed) notify_change();
}

bool wb_biz_send_hello(void)
{
    char out[96];
    size_t n = wb_proto_enc_hello(out, sizeof(out), WB_CAPS);
    return n > 0 && wb_link_send(out, n);
}

bool wb_biz_send_battery(int soc)
{
    char out[64];
    size_t n = wb_proto_enc_battery(out, sizeof(out), soc);
    return n > 0 && wb_link_send(out, n);
}
