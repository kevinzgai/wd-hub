// main/wb_console.c —— 控制台编排层实现。
//
// 职责边界：
//   · 收 BLE 帧 → 解码 → 推进状态机 → 回 ACK
//   · 接按键请求 → 按"当前页"分派语义 → 必要时发上行帧
//   · 周期读电量并上报（主机侧 Web 控制台要显示）
//   · 每帧把状态机快照交给 wb_view 刷新（这里不碰任何 LVGL 控件）
//
// 不负责：帧格式（wb_proto）、业务规则（wb_core）、像素（wb_view）、
// BLE 协议栈（wb_ble）。任何一条出问题都能单独替换。
#include "wb_console.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include "nvs.h"
#include "nvs_flash.h"

#include "audio.h"
#include "battery.h"
#include "board_display.h"
#include "esp_heap_caps.h"
#ifdef WB_KEYS3
#include "ui_board.h"        // 长按 OK 复用看板的亮度档位逻辑（只写请求标志）
#endif
#include "wb_ble.h"
#include "wb_core.h"
#include "wb_proto.h"
#include "wb_view.h"

static const char *TAG = "wb-console";

#define NVS_NS            "wbconsole"
#define NVS_KEY_SKIN      "skin"

#define QUEUE_DEPTH       6
#define TASK_STACK        6144
#define TASK_PRIO         5
#define BATT_POLL_MS      30000
#define UI_IDLE_MS        1000     // 无事件时也每秒刷一次（运行计时/离线角标靠它）
#define EXIT_TO_BOARD_MS  60000    // 断链多久后退回看板屏

// 本固件实际实现的能力位。定义在 wb_proto.h（与 WB_CAP_* 位定义同处），
// 核心层 wb_core.c 也要用同一份真源做多选降级，两边口径必须一致。
// 说明见 wb_proto.h 里 WB_CAPS 的注释。

typedef enum {
    KIND_FRAME = 0,
    KIND_KEY,
    KIND_BATT,
} msg_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t long_press;
    uint16_t len;
    char key[8];
    char data[WB_RECV_MAX + 1];
} console_msg_t;

static wb_state_t s_state;
static QueueHandle_t s_queue;
static volatile bool s_ready;
static volatile bool s_owns_keys;
static volatile bool s_dirty;
#ifdef WB_PREVIEW_DATA
static bool s_preview_active;   // true = 还在看假数据，按键走"遍历七页"
static int  s_preview_page;     // 预览模式当前停在第几屏（0..6）
#endif
static int64_t s_last_linked_ms;
static int64_t s_last_active_ms;
static bool s_standby_dimmed;   // 背光当前是否已经按待机档调暗

// ---------------------------------------------------------------------------
// 投递
// ---------------------------------------------------------------------------
static void post(const console_msg_t *m)
{
    if (!s_queue) return;
    // 队列满说明界面跟不上主机：丢最旧的一条，绝不阻塞 BLE 回调。
    if (xQueueSend(s_queue, m, 0) != pdTRUE) {
        console_msg_t dropped;
        (void)xQueueReceive(s_queue, &dropped, 0);
        (void)xQueueSend(s_queue, m, 0);
    }
}

static void on_ble_frame(const char *frame, size_t len)
{
    if (len > WB_RECV_MAX) return;
    console_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = KIND_FRAME;
    m.len = (uint16_t)len;
    if (frame && len) memcpy(m.data, frame, len);
    post(&m);
}

void wb_console_key(const char *key, bool long_press)
{
    if (!s_ready || !key) return;
    console_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = KIND_KEY;
    m.long_press = long_press ? 1 : 0;
    strncpy(m.key, key, sizeof(m.key) - 1);
    post(&m);
}

// ---------------------------------------------------------------------------
// NVS（只有外观落设备；音量真源在主机，合并镜像会清 NVS）
// ---------------------------------------------------------------------------
static void skin_save(uint8_t skin)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, NVS_KEY_SKIN, skin);
    (void)nvs_commit(h);
    nvs_close(h);
}

static uint8_t skin_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, NVS_KEY_SKIN, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v & 1U;
}

// ---------------------------------------------------------------------------
// 提示音：把控制台的三种语义映射到板上已有的 5 段 PCM 提示音。
// 板子只有"叮/咚/叮叮/哒"这几种现成音色，不为了多一种语义去重烧 PCM。
// ---------------------------------------------------------------------------
static void beep_play(wb_beep_kind_t kind)
{
    if (s_state.beep_vol <= 0) return;   // 主机把音量调到 0 = 静音
    switch (kind) {
    case WB_BEEP_APPROVAL: audio_play(AUDIO_EV_ATTENTION); break;
    case WB_BEEP_ASK:      audio_play(AUDIO_EV_ATTENTION); break;
    case WB_BEEP_DONE:     audio_play(AUDIO_EV_DONE); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// 上行
// ---------------------------------------------------------------------------
static void send_raw(const char *data, size_t len)
{
    if (!data || len == 0) return;

    // 上行帧一律以 '\n' 收尾：主机侧 _on_notify 是按 '\n' 切行解析的，
    // 缺换行的帧会和后面所有帧粘成一行。JSON 帧由 wb_proto_enc_* 自带换行，
    // 但裸文本 ACK（OK_STATUS / OK_APPROVAL / ERR_JSON …）没有 ——
    // 之前十次 ACK 就是因此粘连成 "OK_CFG:80OK_STATUSOK_STATUS…{...}"，
    // 主机 json.loads 失败后整段丢弃，音量回执也就丢了。
    if (data[len - 1] == '\n') {
        if (!wb_ble_send(data, len)) ESP_LOGW(TAG, "上行帧未发出（链路未连接）");
        return;
    }

    // 正常只会走到这里的是短 ACK，32B 足够。
    // 超长的必然是组包出了问题，宁可不发也不能缩帧 —— 主机按定长硬切。
    char line[32];
    if (len + 1 > sizeof(line)) {
        ESP_LOGW(TAG, "上行裸帧过长（%uB）且无换行，丢弃", (unsigned)len);
        return;
    }
    memcpy(line, data, len);
    line[len] = '\n';
    if (!wb_ble_send(line, len + 1)) ESP_LOGW(TAG, "上行帧未发出（链路未连接）");
}

static void send_ack(wb_ev_type_t type)
{
    char scratch[24];
    const char *ack = wb_proto_ack_for(type, scratch, sizeof(scratch), s_state.beep_vol);
    if (ack) send_raw(ack, strlen(ack));
}

// ---------------------------------------------------------------------------
// 帧处理
// ---------------------------------------------------------------------------
static void handle_frame(const char *frame, size_t len, int64_t now)
{
    static wb_ev_t ev;   // 约 1.1KB，不放任务栈上反复压

    if (!wb_proto_decode(frame, len, &ev)) {
        ESP_LOGW(TAG, "无法解析的帧（%u 字节），回 ERR_JSON", (unsigned)len);
        send_raw("ERR_JSON", 8);
        return;
    }
    if (ev.type == WB_EV_NONE) return;   // 语音轮询等不归我们处理，安静忽略

#ifdef WB_PREVIEW_DATA
    if (s_preview_active) {
        s_preview_active = false;
        wb_view_force_page(-1);          // 回到状态机决定页面
        ESP_LOGI(TAG, "收到真实帧，退出预览模式");
    }
#endif

    wb_tick_t side;
    const bool redraw = wb_core_apply(&s_state, &ev, now, &side);
    if (side.beep) beep_play(side.beep_kind);
    if (side.standby_changed && !s_state.standby_page) {
        /* 这条日志是"待机时被新任务叫醒"的验收入口：主机不连时看不到画面变化，
         * 只能靠它确认判定链路（status/approval/ask 帧 → 清待机 → 恢复背光）。 */
        ESP_LOGI(TAG, "检测到新任务/新会话 → 退出待机");
    }

    switch (ev.type) {
    case WB_EV_APPROVAL:
        ESP_LOGI(TAG, "审批 #%d tool=%s hint=%uB", ev.approval_id, ev.tool,
                 (unsigned)strlen(ev.hint));
        break;
    case WB_EV_ASK:
        ESP_LOGI(TAG, "提问 #%d opts=%d ans=%d multi=%d", ev.ask_id, ev.n_opts, ev.ask_ans,
                 ev.ask_multi);
        break;
    case WB_EV_CREDIT:
        s_dirty = true;
        break;
    case WB_EV_CFG:
        if (ev.beep_test) audio_play(AUDIO_EV_CONNECTED);
        break;
    default:
        break;
    }

    send_ack(ev.type);
    if (redraw) s_dirty = true;
}

// ---------------------------------------------------------------------------
// 按键语义
// ---------------------------------------------------------------------------
static void decide_approval(bool allow)
{
    if (!s_state.appr.active) return;
    const int id = s_state.appr.id;
    char buf[96];
    const size_t n = wb_proto_enc_key(buf, sizeof(buf), id, allow ? "ok" : "down");
    send_raw(buf, n);
    ESP_LOGI(TAG, "审批 #%d → %s", id, allow ? "allow" : "deny");
    // 乐观退出：立刻收起页面，避免用户连按两次。
    // 主机若没收到会在自己的超时后 fail-open，不会永远卡住。
    memset(&s_state.appr, 0, sizeof(s_state.appr));
    s_dirty = true;
}

static void submit_ask(void){
    wb_ask_t *a = &s_state.ask;
    if (!a->active) return;
    if (a->ans == 0) {
        ESP_LOGW(TAG, "该提问不可在设备上作答（ans=0）");
        audio_play(AUDIO_EV_KEY);
        return;
    }

    char buf[96];
    if (a->multi) {
        if (a->mask == 0) {
            // 一项没勾就提交 = 无效答案。主机侧会判成"未给出有效选项"并保留当前页，
            // 所以这里直接拒绝，不发出去。
            ESP_LOGW(TAG, "多选未勾选，拒绝提交");
            audio_play(AUDIO_EV_KEY);
            return;
        }
        const size_t n = wb_proto_enc_ask_mask(buf, sizeof(buf), a->id, a->mask);
        send_raw(buf, n);
        ESP_LOGI(TAG, "提问 #%d 多选提交 mask=0x%x", a->id, a->mask);
    } else {
        const size_t n = wb_proto_enc_ask_index(buf, sizeof(buf), a->id, a->cursor);
        send_raw(buf, n);
        ESP_LOGI(TAG, "提问 #%d 单选提交 index=%d", a->id, a->cursor);
    }
    memset(&s_state.ask, 0, sizeof(s_state.ask));
    s_dirty = true;
}


// ---------------------------------------------------------------------------
// 单键语义（本机只有 BOOT 一颗键）。
//
// 设计原则：**短按 = 翻页，长按 = 返回上一页**。
//
// 这是 2026-10-31 的第三次改版。前两版都建立在"短按 = 推进/确认、长按 = 返回"
// 上，结果是进运行详情页要「状态页长按 → 再长按」，中间还不能超过 8 秒；
// 而进会话列表只要一次短按。三条导航路径手势各不相同、有的要两级，
// 用户的原话是「怎么换页呢」—— 得先问，说明它没自解释。
//
// 现在把三个导航页摊平成一条环：
//
//     配额总览 --短按--> 会话列表 --短按--> 运行详情 --短按--> 配额总览
//        └────────────── 长按（任意一页）→ 退回上一页 ──────────────┘
//
// 三条理由：
//   1. **不需要记**。任何一页上短按都往前翻，翻三下必然回到原点，
//      不用知道"我在第几页才能去到哪页"。
//   2. **零延迟**。短按在松手那一刻直接翻页，不像双击/三连击要等
//      一个 400ms 的判定窗口。审批页对延迟敏感，这条不能让步。
//   3. **只有两种手势**。单键板表达不了第三种，多出来的手势必然要
//      靠"空闲自动退出"之类的隐性规则兜底，那些用户看不见。
//
//   审批页   短按 → 允许          长按 → 拒绝
//   提问页   短按 → 下一选项      长按 → 提交当前项
//
// 发语音页已整体下线（2026-10-31 用户决定：暂时只做纯看板 + 审批/提问/
// 完成提醒）。原来留给它的"长按"手势空了出来，还给导航做后退。
//
// 于是不再有"选中会话"这件事：列表页就三张卡，屏幕也点不动，选中之后
// 既不能打开也不能操作，那个高亮一直是纯装饰。这一版把它删了。

// 导航页里"下一页"的那一页。1→2→3→4→1 循环。
//
// 第 4 个导航页是「今日消耗」（WB_PAGE_USAGE）。它排在运行详情之后而不是
// 紧跟配额总览，因为前两页都是"现在正在发生什么"，消耗是回看性质，
// 放在环尾更符合"由近及远"的阅读顺序。
//
// ⚠ 待机页（WB_PAGE_STANDBY）不在这个环里：它是空闲自动切的，
// 手动翻进去就得给一个"怎么翻出去"的手势，而单键板已经没有第四个手势了。
static wb_page_t nav_next_page(wb_page_t cur)
{
    if (cur == WB_PAGE_STATUS) return WB_PAGE_RACK;
    if (cur == WB_PAGE_RACK)   return WB_PAGE_RUN;
    if (cur == WB_PAGE_RUN)    return WB_PAGE_USAGE;
    return WB_PAGE_STATUS;
}

// 长按的后退目标，与 nav_next_page 完全对称。
// （原语义是"进发语音页"，语音下线后这个手势改作导航后退，
// 否则长按在导航页上就是个空动作，用户按了没反应会以为按键坏了。）
static wb_page_t nav_prev_page(wb_page_t cur)
{
    if (cur == WB_PAGE_USAGE) return WB_PAGE_RUN;
    if (cur == WB_PAGE_RUN)   return WB_PAGE_RACK;
    return WB_PAGE_STATUS;
}

static void handle_key(const char *key, bool long_press)
{
#ifndef WB_KEYS3
    (void)key;   // 单键板：key 恒为 "OK"

#ifdef WB_PREVIEW_DATA
    /* 版式自检期间，短按 = 翻到下一屏（七页循环），用来一页页核对排版；
     * 长按 = 试听提示音。一旦收到真实帧就退出这个分支，按键恢复正式语义。 */
    if (s_preview_active) {
        if (long_press) {
            audio_play(AUDIO_EV_ATTENTION);
            ESP_LOGI(TAG, "预览试听：等待操作音");
        } else {
            s_preview_page = (s_preview_page + 1) % 7;
            wb_view_force_page(s_preview_page);
            s_dirty = true;
            ESP_LOGI(TAG, "预览页 -> %d", s_preview_page);
        }
        return;
    }
#endif

    const wb_page_t page = wb_core_page(&s_state);

    switch (page) {
    case WB_PAGE_APPROVAL:
        // 短按允许 / 长按拒绝。审批页一进去就要能一眼看懂，不做二级确认。
        decide_approval(!long_press);
        break;

    case WB_PAGE_ASK:
        if (long_press) {
            s_state.ask.cursor = wb_view_ask_cursor();
            submit_ask();
        } else {
            wb_view_ask_move(1, s_state.ask.n_opts);
            s_dirty = true;
        }
        break;

    case WB_PAGE_STANDBY:
        /* 待机页（大眼睛）：任意按键都只做一件事 —— 唤醒回到主屏。
         *
         * 不在这里分"短按翻页/长按后退"：人刚被叫醒时唯一意图是"我要看东西"，
         * 让他一按就直接落在配额总览，比先猜他想翻到哪页更合理（原看板
         * 休眠屏也是这个约定：按 BOOT 一秒内回到正常界面）。
         *
         * 长按同样只唤醒（不重复动作）—— 长按的额外语义在这里没有意义。 */
        ESP_LOGI(TAG, "待机页按键 → 唤醒回总览");
        wb_core_goto_page(&s_state, WB_PAGE_STATUS);
        s_dirty = true;
        break;

    case WB_PAGE_RACK:
    case WB_PAGE_RUN:
    case WB_PAGE_USAGE:
    case WB_PAGE_STATUS:
    default:
        wb_core_goto_page(&s_state, long_press ? nav_prev_page(page)
                                               : nav_next_page(page));
        s_dirty = true;
        break;
    }
#else   // ---------------------------- C3 三键板 ----------------------------
    const bool ok = (key && strcmp(key, "OK") == 0);
    const bool up = (key && strcmp(key, "UP") == 0);

#ifdef WB_PREVIEW_DATA
    if (s_preview_active) {
        if (long_press) {
            audio_play(AUDIO_EV_ATTENTION);
            ESP_LOGI(TAG, "预览试听：等待操作音");
        } else {
            // 短按 OK/DOWN 往后翻、短按 UP 往前翻，方便一页页核对排版。
            s_preview_page = up ? (s_preview_page + 6) % 7
                                : (s_preview_page + 1) % 7;
            wb_view_force_page(s_preview_page);
            s_dirty = true;
            ESP_LOGI(TAG, "预览页 -> %d", s_preview_page);
        }
        return;
    }
#endif

    const wb_page_t page = wb_core_page(&s_state);

    switch (page) {
    case WB_PAGE_APPROVAL:
        /* 审批页保留原有两种结果（少一个主机就会一直等到超时）：
         *   短按 OK = 允许，长按 OK = 拒绝。
         * UP/DOWN 在这一页不做事，避免误触把审批翻过去。 */
        if (ok) decide_approval(!long_press);
        break;

    case WB_PAGE_ASK:
        /* 提问页：短按 OK = 下一选项（原语义），长按 OK = 提交；
         * 短按 UP/DOWN 在选项之间移动，比只能单向循环更好用。 */
        if (ok && long_press) {
            s_state.ask.cursor = wb_view_ask_cursor();
            submit_ask();
        } else {
            wb_view_ask_move(up ? -1 : 1, s_state.ask.n_opts);
            s_dirty = true;
        }
        break;

    case WB_PAGE_STANDBY:
        /* 待机：任意键唤醒回总览（三键板同样适用）。
         * 背光 50%→100% 由 wb_console_tick 的 standby 边沿负责。 */
        ESP_LOGI(TAG, "待机页按键(%s) → 唤醒回总览", key ? key : "?");
        wb_core_goto_page(&s_state, WB_PAGE_STATUS);
        s_dirty = true;
        break;

    case WB_PAGE_RACK:
    case WB_PAGE_RUN:
    case WB_PAGE_USAGE:
    case WB_PAGE_STATUS:
    default:
        if (long_press) {
            if (ok) {
                /* 长按 OK = 亮度循环。沿用单键板"长按调亮"的已验收语义：
                 * 只写请求标志，由 LVGL 任务在 poll_input 里落实。 */
                ui_board_brightness("OK", 0);
            } else {
                /* 长按 DOWN（或 UP 已在驱动层被配网吃掉）= 返回首页/退出浮层。 */
                wb_core_goto_page(&s_state, WB_PAGE_STATUS);
                s_dirty = true;
            }
        } else if (ok) {
            // 短按 OK：唤醒/回到总览。
            wb_core_goto_page(&s_state, WB_PAGE_STATUS);
            s_dirty = true;
        } else {
            // 短按 UP = 上一页，短按 DOWN = 下一页（四页环）。
            wb_core_goto_page(&s_state, up ? nav_prev_page(page)
                                           : nav_next_page(page));
            s_dirty = true;
        }
        break;
    }
#endif  // WB_KEYS3
}

// ---------------------------------------------------------------------------
// 电量
// ---------------------------------------------------------------------------
static void poll_battery(void)
{
    battery_state_t bs;
    battery_get(&bs);
    if (!bs.valid || bs.percent < 0) return;
    s_state.soc = bs.percent;

    /* 只在链路活着时上行。
     *
     * 电量是 30 秒一次的周期上报，没有主机时发出去必然失败 —— 原来每 30 秒
     * 一条「上行帧未发出（链路未连接）」warning，跑一晚上就是 2880 条，
     * 把真正有用的日志全冲掉了。链路上没有接收方时这次上报本来就没有意义，
     * 直接不发比"发了再抱怨发不出去"更合理。
     *
     * 注意 update 本地状态的部分在上面，不受这个判断影响：
     * 屏幕上的电量角标在无主机时照样要更新。
     */
    bool can_send = s_state.linked;
#ifdef WB_PREVIEW_DATA
    // 预览模式的 linked 是假数据写死的 true，实际并没有主机。
    // 不加这个判断，预览跑一晚上会刷出几千条无意义 warning。
    if (s_preview_active) can_send = false;
#endif
    if (!can_send) {
        s_dirty = true;
        return;
    }

    char buf[48];
    const size_t n = wb_proto_enc_battery(buf, sizeof(buf), s_state.soc);
    send_raw(buf, n);
    s_dirty = true;
}

// ---------------------------------------------------------------------------
// 任务
// ---------------------------------------------------------------------------
static void console_task(void *arg)
{
    (void)arg;
    int64_t last_batt = 0;

    for (;;) {
        console_msg_t m;
        const bool got = (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(100)) == pdTRUE);
        const int64_t now = esp_timer_get_time() / 1000;

        if (got) {
            if (m.kind == KIND_FRAME) handle_frame(m.data, m.len, now);
            else if (m.kind == KIND_KEY) handle_key(m.key, m.long_press != 0);
            else poll_battery();
        }

        // 连上之后补发 hello：告诉主机本固件认哪些帧（caps 位）。
        if (wb_ble_take_hello()) {
            char buf[96];
            const size_t n = wb_proto_enc_hello(buf, sizeof(buf), WB_CAPS);
            send_raw(buf, n);
            s_last_linked_ms = now;
            ESP_LOGI(TAG, "已发送 hello: fw=%s proto=%d caps=0x%x", WB_FW_VERSION,
                     WB_PROTO_VERSION, (unsigned)WB_CAPS);
        }

        if (now - last_batt >= BATT_POLL_MS) {
            last_batt = now;
            poll_battery();
        }

        // 状态机周期推进（离线判定、审批/提问超时、自动切页）。
#ifdef WB_PREVIEW_DATA
        // 预览期间不停刷新"最后收帧时间"，否则 6 秒后就被判离线、
        // 界面切到 OFFLINE 态，版式看不清。
        s_state.last_rx_ms = now;
#endif
        wb_tick_t t;
        wb_core_tick(&s_state, now, &t);
        if (t.beep) beep_play(t.beep_kind);
        if (t.standby_changed) {
            /* 进/出待机页打一条日志。这不是调试残留：待机页整屏只有眼睛，
             * 现场"屏幕是不是坏了"没法从画面判断，日志是唯一的排查入口
             * （也会记下空闲时长，能验证 3 分钟门槛是否按预期走）。 */
            ESP_LOGI(TAG, "%s待机页（空闲 %llds）",
                     s_state.standby_page ? "进入" : "退出",
                     s_state.idle_since_ms > 0
                         ? (long long)((now - s_state.idle_since_ms) / 1000) : 0);
        }
        if (t.redraw) s_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// 对外
// ---------------------------------------------------------------------------
void wb_console_tick(int64_t now_ms)
{
    if (!s_ready) return;

    /* 待机亮度：只跟随 standby_page 的边沿，避免每帧重复写 LEDC。
     * 待机 50%（用户要求），唤醒回正常 100%（控制台目前没有别的用户档位）。
     * 这里由 LVGL 任务调用，和主循环里的 50%→10fps 降帧一起构成"屏幕侧低功耗"。 */
    const bool standby = s_state.standby_page;
    if (standby != s_standby_dimmed) {
        s_standby_dimmed = standby;
        board_display_backlight_set(standby ? 50 : 100);
        /* 与背光同一个边沿切换 BLE 档位：
         *   待机 200~250ms interval → 激活 30~50ms interval。
         * 唤醒时先恢复到"正常"档再刷页面，审批/按键回传不会带着省电参数跑。 */
        wb_ble_set_power_mode(standby);
        ESP_LOGI(TAG, "背光 → %u%%（%s）", standby ? 50 : 100,
                 standby ? "待机" : "正常");
    }
    wb_ble_poll();

    if (s_state.linked && !s_state.offline) {
        s_last_linked_ms = now_ms;
        s_last_active_ms = now_ms;
    }

#ifndef WB_BLE_ONLY
    // 断链太久就退回看板屏：控制台此刻没有任何可显示的内容，
    // 占着屏幕反而挡住了 WiFi 状态。再来帧会自动切回来。
    if (wb_view_active() && s_state.offline &&
        s_last_linked_ms > 0 && now_ms - s_last_linked_ms > EXIT_TO_BOARD_MS) {
        ESP_LOGI(TAG, "断链 %llds，退回看板屏", (long long)((now_ms - s_last_linked_ms) / 1000));
        wb_view_set_active(false);
    }
#else
    /* 只走蓝牙：没有看板可退（WiFi 都没开，退回去只是一张显示 "--" 的空屏）。
     * 断链就停在当前这一页显示离线态，主机一个 notify 就能把它唤回来。 */
#endif

    // 有数据才抢屏（wb_view 内部对 !linked 也会拦一道）
    wb_view_refresh(&s_state, now_ms);
    s_owns_keys = wb_view_active();
    s_dirty = false;
}

bool wb_console_active(void) { return s_owns_keys; }

bool wb_console_standby_active(void)
{
    return s_ready && s_state.standby_page;
}

void wb_console_toggle_skin(void)
{
    const uint8_t skin = (uint8_t)((wb_view_skin() + 1U) & 1U);
    skin_save(skin);
    wb_view_set_skin(skin);
    s_state.skin = skin;

    // 重建整套界面（低频动作）。这里仍然处在 lvgl_task 的调用栈里
    // （tick 由它驱动），所以直接重建是安全的。
    wb_view_teardown();
    wb_view_build();
    wb_view_set_active(true);
    s_dirty = true;
    ESP_LOGI(TAG, "外观切换为 %s", skin ? "像素风" : "赛博 HUD");
}

void wb_console_start(void)
{
    const int64_t now = esp_timer_get_time() / 1000;

    wb_core_init(&s_state, now);
    s_state.skin = skin_load();
    wb_view_set_skin(s_state.skin);
    wb_view_build();

#ifdef WB_PREVIEW_DATA
    /* 版式自检（cmake -DWB_PREVIEW_DATA=1）：
     * 没有主机时也把界面画出来，用来核对七页的排版/截断/重叠。
     * 一旦收到真实帧，这些假数据会被直接覆盖，不需要改代码切回。 */
    s_state.linked = true;
    s_state.n_sess = 3;
    static const char *k_state[3] = {"RUNNING", "NEED_APPROVAL", "COMPLETED"};
    /* 名字上限看固件 WB_NAME_LEN=48（≈15 汉字），超过会被 snprintf 截断。
     * 第 1 条用 13 个字，在不被截断的前提下尽量贴近这个上限，
     * 顺便验证接近满宽时"序号 / 状态 / 额度"那行还排得开。 */
    static const char *k_name[3] = {"重构登录流程并补齐鉴权测试",
                                    "部署到测试环境", "写周报"};
    static const char *k_model[3] = {"claude-sonnet-4", "claude-sonnet-4", "gpt-5"};
    static const double k_credit[3] = {12.3, 3.1, 0.8};
    /* 故意让中间那条带 msg、另两条不带：第 2 屏的 msg 行是"永远占位"的，
     * 空/非空两种状态必须共用同一套版式，这样一眼就能看出卡片有没有跳动。 */
    static const char *k_msg[3] = {"", "等待审批 bash 命令", ""};
    for (int i = 0; i < 3; i++) {
        snprintf(s_state.sess[i].state, WB_STATE_LEN, "%s", k_state[i]);
        snprintf(s_state.sess[i].name, WB_NAME_LEN, "%s", k_name[i]);
        snprintf(s_state.sess[i].model, WB_MODEL_LEN, "%s", k_model[i]);
        snprintf(s_state.sess[i].msg, WB_NAME_LEN, "%s", k_msg[i]);
        s_state.sess[i].credit = k_credit[i];
    }
    s_state.credit.valid = true;
    s_state.credit.remain = 742.5;
    s_state.credit.total = 1000.0;
    s_state.credit.level = wb_credit_level(742.5, 1000.0, 0);
    s_state.soc = 52;

    /* 审批页 / 提问页只有主机推帧才会出现，但排版同样要验 ——
     * 尤其提问页有 6 个选项 + 正文换行，最容易溢出。这里放一份假数据，
     * 配 wb_view_force_page() 让长按能一页页翻过去看。 */
    s_state.appr.active = true;
    s_state.appr.id = 17;
    snprintf(s_state.appr.tool, WB_TOOL_LEN, "bash");
    snprintf(s_state.appr.hint, WB_HINT_LEN,
             "rm -rf /tmp/build && cargo build --release --features gpu");
    /* 预览数据的超时给足一天：这是给人反复长按翻页核对排版用的，
     * 95 秒后 wb_core_tick() 会把 appr/ask 清空，翻到那一页就是空白。 */
    s_state.appr.deadline_ms = now + 86400000;

    s_state.ask.active = true;
    s_state.ask.id = 5;
    s_state.ask.ans = 1;
    s_state.ask.n_opts = 6;
    snprintf(s_state.ask.tool, WB_TOOL_LEN, "AskUserQuestion");
    snprintf(s_state.ask.text, WB_TEXT_LEN, "选哪个环境部署？");
    snprintf(s_state.ask.opt[0], WB_OPT_LEN, "staging 测试环境");
    snprintf(s_state.ask.opt[1], WB_OPT_LEN, "production 生产环境");
    snprintf(s_state.ask.opt[2], WB_OPT_LEN, "本地 docker");
    snprintf(s_state.ask.opt[3], WB_OPT_LEN, "跳过，先不改");
    snprintf(s_state.ask.opt[4], WB_OPT_LEN, "让我再看看日志");
    snprintf(s_state.ask.opt[5], WB_OPT_LEN, "全都要");
    /* deadline_ms 必须设！wb_core_tick() 里是
     *   if (st->ask.active && now_ms >= st->ask.deadline_ms) memset(&st->ask, 0, ...);
     * 留 0 就等于"第一次 tick 就过期"，整页被清空 ——
     * 表现就是标题/正文/六个选项全空（用户反馈：第 5 屏内容没显示）。
     * 真正的主机推帧会带 TTL，所以这只是预览数据的坑。 */
    s_state.ask.deadline_ms = now + 86400000;

    /* 第 4 屏（今日消耗）的假数据。用真实量级而不是"123"这种小数字 ——
     * token 数缩写（K/M）和横条比例都是按量级分档的，给小数字测不出
     * "1.2M" 这种 4 字符会不会撑破 36px 的数值槽，也测不出五条横条
     * 在长短差距很大时会不会挤在一起。
     *
     * 名字故意留一个超长的（custom-local:MiniMax-M3.1-Flash，28 字符），
     * 验证主机那套"首尾保留 + 中间省略"的截断在 100px 槽里是否够看。 */
    s_state.usage.valid = true;
    s_state.usage.today = 3560720;
    s_state.usage.req = 32;
    s_state.usage.cached = 1425600;
    s_state.usage.n_model = 5;
    {
        static const char *k_um[5] = {"trae/d…4.1-flash", "deepse…4.1-flash",
                                      "deepseek-flash", "qodercn/dfmodel",
                                      "custom…M3.1-Flash"};
        static const long long k_uv[5] = {1187633, 889009, 719828, 526343, 237907};
        static const long long k_uh[5] = {0, 602240, 358784, 374144, 90432};
        for (int i = 0; i < 5; i++) {
            snprintf(s_state.usage.model[i].name,
                     sizeof(s_state.usage.model[i].name), "%s", k_um[i]);
            s_state.usage.model[i].tokens = k_uv[i];
            s_state.usage.model[i].cached = k_uh[i];
        }
    }

    s_preview_active = true;
    wb_view_force_page(0);
    s_last_linked_ms = now;
    wb_view_set_active(true);
    ESP_LOGW(TAG, "WB_PREVIEW_DATA 已启用：界面用假数据渲染，收到真实帧即覆盖");
#endif

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(console_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "队列创建失败，控制台不可用");
        return;
    }

    /* 栈放 PSRAM：开蓝牙后内部 RAM 只剩十几 KB，8192×2 的看板任务已经
     * 起不来了（实测 "lvgl 任务创建失败"）。控制台任务只做 JSON 解析和
     * 状态机推进，对栈访问延迟不敏感，放 PSRAM 最划算。
     * S3 的 Octal PSRAM @80MHz 带宽足够，代价可以忽略。 */
    if (xTaskCreateWithCaps(console_task, "wb_console", TASK_STACK, NULL, TASK_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败，控制台不可用");
        return;
    }

    const esp_err_t err = wb_ble_start(on_ble_frame);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 启动失败: %s（界面仍可用，但收不到主机数据）",
                 esp_err_to_name(err));
    }

#ifdef WB_BLE_ONLY
    /* 只走蓝牙：没有看板可退，开机就占屏显示离线态。
     *
     * 必须赶在 s_ready = true 之前置 —— tick 一跑起来就会走 wb_view_refresh，
     * 那时若还是"要 linked 才抢屏"，屏幕会一直停在那张空看板上
     * （WiFi 没开，看板取不到数，只有一片 "--"）。 */
    wb_view_set_always_on(true);
    ESP_LOGI(TAG, "常驻屏已开：未连主机时显示离线态");
#endif

    s_ready = true;
    ESP_LOGI(TAG, "WorkBuddy 控制台就绪：外观=%s 固件=%s",
             s_state.skin ? "像素风" : "赛博 HUD", WB_FW_VERSION);
}
