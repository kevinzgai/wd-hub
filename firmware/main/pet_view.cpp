/* 像素宠物视图 —— 实现（从 main.cpp 搬出，状态机与换帧逻辑未改） */
#include "pet_view.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "audio.h"
#include "pet.h"

static const char *TAG = "pet";

// ---- 位置与透明度 ----
// 素材是 PET_IMG_W x PET_IMG_H = 120x96（不是 120x120 —— 之前这里的注释
// 写错了，描述符实际 .h=96）。BOTTOM_RIGHT 对齐 + 偏移 (4, 4)：
//   对象右下角对齐屏幕右下角，再往右下各挪 4px
//   => 占 x=124..243、y=148..243
// 屏幕可见区是 0..239，所以右侧 4px、底部 4px 出屏被裁。
// 故意让它出屏一点：像素画的边缘是硬直边，正好压在屏幕边框上会像
// "贴了张方块贴纸"；溢出一点被裁掉后，视觉上更像浮在角落。
// 底部裁掉的是头盔最下沿那 1 行暗边（逻辑行 30），不影响观感。
#define PET_X           (4)
#define PET_Y           (4)
// 半透明。实测数据（sRGB 线性化 + WCAG 公式）：
//   LV_OPA_30(29.8%) -> 白头盔上屏 #4B4D52，与底色 #06080C 对比 2.37:1
//                       低于 WCAG 非文本图形 3:1 门槛，猫脸糊成一团
//   LV_OPA_40(40.0%) -> 上屏 #62656A，3.43:1 过线；压在猫身上的
//                       COL_DIM(#B4BEC9) 文字仍有 3.11:1，勉强够用
//   LV_OPA_45(45.1%) -> 猫更清楚，但文字掉到 2.60:1，会话列表开始糊
// 所以 40% 是这个布局的实际拐点：再高就伤到要读的文字了。
// 注意这里混的是屏幕背景色，不是上面的文字 —— 宠物在最底层，
// 文字画在它上面，但页脚/会话用的是灰色(COL_DIM)，压在白色猫身上
// 对比度天生不足，所以只能把猫本身压淡来让文字浮出来。
// LV_OPA_40 = 102（范围 0..255）。
#define PET_OPA         LV_OPA_40

// 四种状态对应四种"心情"，换帧周期也不同 —— 越忙动得越快，静止时慢慢眨眼，
// 一眼就能看出机器在干什么，不用去读文字。
static const struct {
    uint16_t period_ms;
    const char *name;
} PET_CFG[PET_STATES] = {
    [PET_ST_IDLE]    = { 1600, "idle"    },   // 没人用：慢眨眼，像在喘气
    [PET_ST_WORKING] = {  500, "working" },   // 干活中：眯眼盯屏，动得快
    [PET_ST_DONE]    = {  600, "done"    },   // 刚完成：张口笑，一开一合
    [PET_ST_AUTO]    = {  800, "auto"    },   // 自动化：耳朵被压，中速
};

static lv_obj_t   *s_pet_img    = NULL;
static lv_timer_t *s_pet_timer  = NULL;   // 换帧定时器，切状态时要改周期
static uint8_t     s_pet_state  = PET_ST_IDLE;
static uint8_t     s_pet_frame  = 0;

static void pet_apply_frame(void)
{
    if (!s_pet_img) return;
    // 隐藏时不换帧：lv_image_set_src 会触发重绘，图表屏上宠物不可见，
    // 白刷一次 SPI 没有任何意义。
    if (lv_obj_has_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN)) return;
    // 帧表排布 pet_frames[state * PET_FRAMES_EACH + frame]
    const lv_image_dsc_t *dsc =
        &pet_frames[s_pet_state * PET_FRAMES_EACH + s_pet_frame];
    // 切帧 = 换指针。图像本身在 flash 里，不分配、不拷贝。
    lv_image_set_src(s_pet_img, dsc);
}

// 换帧定时器。周期跟着状态走，由 pet_view_set_state 负责改，这里只管翻帧。
// （LVGL 9 只有 lv_timer_set_period，没有 get_period，不要试图在这里回读校验。）
static void pet_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_pet_state >= PET_STATES) s_pet_state = PET_ST_IDLE;
    s_pet_frame ^= 1;                       // 0 <-> 1，两帧循环
    pet_apply_frame();
}

void pet_view_set_state(uint8_t st)
{
    // 同状态重复调用直接返回，避免每轮刷新都重设图像源。
    if (st >= PET_STATES || st == s_pet_state) return;
    s_pet_state = st;
    s_pet_frame = 0;
    pet_apply_frame();

    // 周期跟着状态换，并且立刻重置计时。不 reset 的话，从 idle(1600ms)
    // 切到 working(500ms) 得等这一轮剩余的 1600ms 走完才动第一帧，
    // 用户看到的就是"有任务了但宠物愣了一秒多"。
    if (s_pet_timer) {
        lv_timer_set_period(s_pet_timer, PET_CFG[st].period_ms);
        lv_timer_reset(s_pet_timer);
    }
}

void pet_view_set_visible(bool on)
{
    if (!s_pet_img) return;
    if (on) {
        lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN);
        // 隐藏期间 pet_apply_frame 是跳过的（省 SPI），重新显示时
        // 图像源可能停在隐藏前的那一帧。补一次刷新，让表情立刻对上
        // 当前状态，而不是等下一个换帧周期。
        pet_apply_frame();
    } else {
        lv_obj_add_flag(s_pet_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void pet_view_build(lv_obj_t *scr)
{
    // 素材是 ARGB8888（带真 alpha），所以可以整体设不透明度而不产生黑罩：
    // 透明区 alpha=0 完全透出下层，只有猫本身半透明。
    s_pet_img = lv_image_create(scr);
    lv_image_set_src(s_pet_img, &pet_frames[PET_ST_IDLE * PET_FRAMES_EACH]);

    // 不做缩放。素材本身就是 120x96，1:1 绘制。
    //
    // 上一版用 lv_image_set_scale(512) 想把 60x60 放大到 120，屏幕上只露出
    // 左上角一块。原因是 set_scale 只改"绘制变换"，对象尺寸仍是 60x60，
    // 而 pivot 默认在图像中心，放大后图像以中心向四周各溢出 30px，
    // 超出对象边界就被裁掉。改素材尺寸比改 pivot/size 组合可靠得多。

    // 半透明。注意用 image_opa 而不是 obj_set_style_opa：
    // 后者会把整个对象（含可能的子对象）整体合成一次，开销更大；
    // image_opa 直接作用在图像绘制阶段，正是我们要的效果。
    lv_obj_set_style_image_opa(s_pet_img, PET_OPA, 0);

    // 不让它拦截按键/点击，纯装饰
    lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_pet_img, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_align(s_pet_img, LV_ALIGN_BOTTOM_RIGHT, PET_X, PET_Y);

    // 定时换帧，间隔按状态在 pet_view_set_state 里动态改
    s_pet_timer = lv_timer_create(pet_timer_cb, PET_CFG[PET_ST_IDLE].period_ms, NULL);

    // 压到最底层：作为屏幕的第 0 个子对象，先于所有内容绘制。
    //
    // LVGL 没有 z-index，子对象顺序就是绘制顺序 —— index 0 最先画（最底），
    // index 越大越靠上。所以这里显式移到 0，让页头、三格统计、会话列表、
    // 页脚文字全都盖在它上面。
    //
    // 用 lv_obj_move_to_index 而不是 lv_obj_move_background：
    // 后者在 LVGL 9 里只是 api_map_v8.h 里的 static inline 兼容别名
    // （内部就是 move_to_index(obj, 0)），直接用新名字更明确。
    lv_obj_move_to_index(s_pet_img, 0);
}

// "刚完成"高亮：today_done 增加时记一个到期时间戳，8 秒内保持 DONE 表情。
// 用时间戳而不是计数器，是为了不依赖刷新频率（HTTP 轮询间隔可能变）。
#define PET_DONE_HOLD_MS  8000
static int64_t s_done_until_ms = 0;
static int     s_prev_today_done = -1;

// 上一轮的"有没有会话在等操作"。语音提示靠它的**上升沿**触发。
// 用边沿而不是电平，因为服务端 5 秒推一轮，等待状态通常持续好几分钟 ——
// 用电平的话会每 5 秒叫一次，夜里能把人吵醒。
static bool s_prev_any_waiting = false;

// 上一轮的活跃会话数，用于检测"会话收工"（working 减少）。
//
// ⚠️ 这里有个必须处理的数据特性：working 的 SQL 是
//     LOWER(status) IN ('working','active') AND last_activity_at >= now-30min
// 也就是说会话超过 30 分钟没动静，会自动滑出计数 —— 这不是"完成"，
// 只是"过期"。如果直接对 working 减少就响铃，那么任何一次长时间离开
// 电脑再回来，都会听到莫名其妙的"完成"提示。
//
// 对策：减少时再确认一次 today_done 没有同时变化（那是真的完成 todo，
// 已经由上面分支响过 DONE 音了），并且要求减少后仍有数据、非首次同步。
// 更关键的是这里**不做任何猜测性播放**：只把减少也算作"有进展"，
// 让它和 todo 完成共用去抖窗口，短时间内只响一次，避免连响。
static int  s_prev_working = -1;

void pet_view_update(const board_data_t *d)
{
    // 只有概览屏（第 0 屏，走 /status）的数据里带 working / today_done / sessions。
    // 详情屏（/screen/<i>）只下发 rows，这三个字段在解析里 fallback 成 0。
    //
    // 如果照旧参与判断，切一次屏就会：
    //   概览(working=2, done=3) -> 详情(0, 0)  : working 减少 → 误响"收工"
    //   详情(0, 0) -> 概览(2, 3)               : done 增加   → 误响"完成"
    // 也就是每切一圈屏白响两声。加音频之前只表现为表情闪一下，
    // 现在会变成实体噪音，所以非第 0 屏直接跳过，宠物保持原状态不动。
    if (d->page != 0) return;

    int64_t now = esp_timer_get_time() / 1000;

    // 本轮是否已经因为"todo 完成"响过。用于避免同一轮里
    // DONE 和 SESSION_END 连着响两声。
    bool played_done = false;

    // 首次拿到数据时只记基线，不触发完成动画 —— 否则每次开机都要先笑一下
    if (s_prev_today_done < 0) {
        s_prev_today_done = d->today_done;
        s_prev_working    = d->working;
        ESP_LOGI(TAG, "基线: done=%d working=%d",
                 s_prev_today_done, s_prev_working);
    } else if (d->today_done > s_prev_today_done) {
        // todo 勾完一项或多项。
        // 一次完成多项（0 -> 3）也只响一次：连响三次纯属噪音，
        // "刚才有进展"这一条信息响一声就够了。
        ESP_LOGI(TAG, "完成: done %d -> %d，播放完成音",
                 s_prev_today_done, d->today_done);
        s_done_until_ms = now + PET_DONE_HOLD_MS;
        s_prev_today_done = d->today_done;
        audio_play(AUDIO_EV_DONE);
        played_done = true;
    } else if (d->today_done < s_prev_today_done) {
        ESP_LOGI(TAG, "基线回退: done %d -> %d",
                 s_prev_today_done, d->today_done);
        s_prev_today_done = d->today_done;   // 跨天归零，跟一下基线
    }

    // 会话收工：working 减少。
    //
    // ⚠️ working 的 SQL 带 30 分钟活跃窗口：
    //     LOWER(status) IN ('working','active') AND last_activity_at >= now-30min
    // 所以会话只是"久没动静"也会滑出计数，那不是完成。
    // 完全靠这个信号会误报，因此：
    //   - 本轮已经响过 DONE（真的完成了 todo）就不再响，避免连响两声
    //   - 用 played_done 判断而不是比较 s_prev_today_done ——
    //     后者在上一分支里已经更新过了，比较永远成立，是个隐蔽的 bug
    if (s_prev_working >= 0 && d->working < s_prev_working && !played_done) {
        audio_play(AUDIO_EV_SESSION_END);
    }
    s_prev_working = d->working;

    // 有没有会话在等用户操作 —— 比"有几个自动化"重要得多
    bool any_waiting = false;
    for (int i = 0; i < d->session_count; i++) {
        if (d->sessions[i].waiting) { any_waiting = true; break; }
    }

    // 只在"从无到有"那一刻响。持续等待期间不重复提示。
    if (any_waiting && !s_prev_any_waiting) {
        audio_play(AUDIO_EV_ATTENTION);
    }
    s_prev_any_waiting = any_waiting;

    // 优先级：刚完成 > 有会话在等 > 有任务在跑 > 待机
    //
    // 注意不能拿 d->automations 判断"自动化在跑"：
    // 那个字段是"已启用的自动化总数"，配置了就一直 >0（当前机器是 5），
    // 用它做条件等于永远进不了 IDLE，宠物会一直抖耳朵。
    // AUTO 只用于"确实没有会话、但也不该显示纯待机"的兜底，这里索性并入
    // WORKING 之外的判断 —— 让 idle 真的能出现。
    uint8_t st;
    if (now < s_done_until_ms)     st = PET_ST_DONE;
    else if (any_waiting)          st = PET_ST_AUTO;     // 耳朵压扁：等你操作
    else if (d->working > 0)       st = PET_ST_WORKING;  // 干活中
    else                           st = PET_ST_IDLE;

    pet_view_set_state(st);
}
