// main/wb_eyes.c —— 眼睛动画实现。
//
// 造型来自一张参考图，四个特征缺一不可：
//   1) 眼白不是椭圆，是横向椭圆被**一条斜睑线**从上方切掉之后的残月 ——
//      外角高、内角低、中段微微下凸。这条斜线是"眼睛"和"半个饼"的分界。
//   2) 瞳孔贴住上睑、被切掉上半，于是和纯黑背景连成一片。
//   3) 眼白下缘一条浅灰带，是"眼球厚度"，不是投影。
//   4) 瞳孔里一枚小白方块 —— 缺了它，瞳孔就只是个洞。
//   眉毛是独立一条暗色弧线，压在纯黑上只比背景亮一档，眉尾向外上挑。
//
// 为什么不用 lv_obj 拼：斜睑线是整个设计的核心，而圆角矩形只能做胶囊/圆角，
// 摆不出斜线（LVGL 9 也砍掉了对象旋转）。所以走 lv_canvas 逐像素画。
//
// 代价与对策：
//   画布 204x82 RGB565 = 33KB，落在 PSRAM；每帧只重算有内容的列。
//   **静止时不重算也不 invalidate** —— 待机屏绝大多数时间停在这个状态
//   （没眨眼、视线没漂、呼吸没跨过 1px），所以平均开销接近 0。
//   真正耗电的只有眨眼那 200ms 和视线漂移那 0.5s。
//
// 代码从 ui_board.cpp 原样搬出（像素算法一字未改），只把：
//   · 私有的 s_* 变量改成模块静态（外部不可见，行为不变）
//   · TAG 从 ui_board 改成 wb-eyes
//   · 画布位置/尺寸改引用 wb_eyes.h 的宏（值与原来完全相同）
//
#include "wb_eyes.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "wb-eyes";

#define EYE_W          92      // 单眼宽（椭圆直径）
#define EYE_H          54      // 单眼高
#define EYE_R          20      // 瞳孔边长
#define EYE_HI          8      // 高光边长
#define EYE_CX_L       64      // 左眼中心 x
#define EYE_CX_R      176      // 右眼中心 x
#define EYE_CY         66      // 眼睛中心 y

// 上睑线两端相对椭圆上沿的下移量（全开时）
#define LID_OUT_OPEN    2      // 外角：几乎贴顶
#define LID_IN_OPEN    18      // 内角：压得低 → 斜线
#define LID_BULGE       1      // 中段下凸
// 闭合时相对椭圆下沿的上移量
#define LID_OUT_CLOSE   8
#define LID_IN_CLOSE    4

// 眉毛
#define BROW_THICK      3      // 线粗
#define BROW_GAP_OUT    4      // 外角处与睑线的间隙
#define BROW_GAP_IN    11      // 内角处间隙
#define BROW_LEVEL     58      // 灰度 0-255，压在纯黑上刚好能看出层次
#define BROW_TILT       5      // 眉尾上挑量

// 画布覆盖区域（设备坐标）。上下各留余量给呼吸浮动。
#define EYES_BX     WB_EYES_X
#define EYES_BY     WB_EYES_Y
#define EYES_BW     WB_EYES_W
#define EYES_BH     WB_EYES_H

#define EYES_MS        50      // 20fps 节拍。眼睛不需要 25fps，省一点 SPI
#define EYE_SLIDE_MAX_MS 250   // 单帧最多推进 250ms（超过就按 50ms 算）
#define EYE_CLOSE_MS   70
#define EYE_HOLD_MS    60
#define EYE_OPEN_MS    70
#define DOZE_AFTER_MS  (3 * 60 * 1000)   // 待机满 3 分钟开始犯困
#define EYE_PI         3.14159265f

static lv_obj_t   *s_cv_eyes   = NULL;
static lv_timer_t *s_eye_timer = NULL;
static uint16_t   *s_eye_buf   = NULL;
static uint32_t    s_eye_last_ms = 0;   // 上一帧的时间戳，用来算真实 dt

enum { EYES_IDLE, EYES_CLOSING, EYES_HOLD, EYES_OPENING };

static struct {
    float    open_pct, base_pct, phase_from;
    float    look_x, look_y, look_tx, look_ty;
    uint8_t  phase;
    uint32_t phase_ms, blink_in_ms, look_in_ms;
    uint32_t nod_in_ms, nod_left_ms, awake_ms, ms;
    bool     doze;
    // 上一帧真画出去的值，用来判断这一帧要不要重算
    float    last_open, last_lx, last_ly;
    int      last_breathe;
} s_eyes;

static inline float eye_clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float eye_min(float a, float b) { return a < b ? a : b; }
static inline float eye_max(float a, float b) { return a > b ? a : b; }
static inline float eye_lerp(float a, float b, float p) { return a + (b - a) * p; }

// 灰度 → RGB565（r/g/b 同值）。LVGL 9 的 RGB565 缓冲是 native 小端 uint16，
// 显示层如果是 SWAPPED 格式，由 LVGL 绘制时自己换 —— 这里不用管。
static inline uint16_t eye_grey(int v)
{
    uint16_t g = (uint16_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    return (uint16_t)(((g & 0xF8) << 8) | ((g & 0xFC) << 3) | (g >> 3));
}

static inline void eye_put(int x, int y, int v)
{
    if (x < 0 || x >= EYES_BW || y < 0 || y >= EYES_BH) return;
    s_eye_buf[y * EYES_BW + x] = eye_grey(v);
}

// 像素 [y, y+1) 与区间 [a,b] 的重叠比例，就是抗锯齿用的覆盖率。
// 逐像素画之所以还能不锯齿，全靠这个 —— 边界那一行按覆盖比例混色。
static inline float eye_cover(int y, float a, float b)
{
    float lo = eye_max((float)y, a);
    float hi = eye_min((float)y + 1.0f, b);
    return hi > lo ? hi - lo : 0.0f;
}

// ---- 眉毛 ----
// 基准取"全开"时的睑线，不跟眨眼上下动。真实眨眼时眉毛几乎不动，
// 若让它跟着睑线压，整张脸会像在抽搐。
static void eye_draw_brow(int cx, int cy, bool mirror)
{
    const float rw  = EYE_W * 0.5f;
    const float top = (float)cy - EYE_H * 0.5f;
    float y_out = top + LID_OUT_OPEN;
    float y_in  = top + LID_IN_OPEN;
    // 统一成"从左到右"再按镜像换端点
    float yl = mirror ? y_in : y_out;
    float yr = mirror ? y_out : y_in;

    for (int x = (int)floorf((float)cx - rw); x <= (int)ceilf((float)cx + rw); x++) {
        float t   = eye_clamp(((float)x + 0.5f - ((float)cx - rw)) / (float)EYE_W, 0.0f, 1.0f);
        float gap = mirror ? eye_lerp(BROW_GAP_IN, BROW_GAP_OUT, t)
                           : eye_lerp(BROW_GAP_OUT, BROW_GAP_IN, t);
        // 眉尾：越靠外角越往上翘，三次方衰减，才像笔锋而不是斜棍
        float u    = mirror ? 1.0f - t : t;          // 0 = 外角
        float tilt = (1.0f - u) * (1.0f - u) * (1.0f - u) * BROW_TILT;
        float lid  = yl + (yr - yl) * t + LID_BULGE * sinf(EYE_PI * t);
        float yb   = lid - gap - tilt;               // 眉底
        float ya   = yb - BROW_THICK;                // 眉顶

        // 两端各 18% 收成笔锋
        float alpha = eye_clamp(eye_min(t, 1.0f - t) / 0.18f, 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;

        for (int y = (int)floorf(ya); y <= (int)floorf(yb); y++) {
            float av = eye_cover(y, ya, yb);
            if (av <= 0.0f) continue;
            eye_put(x, y, (int)(BROW_LEVEL * av * alpha + 0.5f));
        }
    }
}

// ---- 一只眼 ----
// pct: 0 = 全闭（只剩底部细月牙），100 = 常态弯月
static void eye_draw_eye(int cx, int cy, float pct, float lx, float ly, bool mirror)
{
    const float rw = EYE_W * 0.5f;
    const float rh = EYE_H * 0.5f;
    const float q  = 1.0f - eye_clamp(pct, 0.0f, 100.0f) * 0.01f;   // 闭合量

    float y_out = eye_lerp((float)cy - rh + LID_OUT_OPEN, (float)cy + rh - LID_OUT_CLOSE, q);
    float y_in  = eye_lerp((float)cy - rh + LID_IN_OPEN,  (float)cy + rh - LID_IN_CLOSE,  q);
    // 闭得越紧弧越小：不然闭眼时中段的凸起会顶出底边，变成一道黑线
    float bulge = LID_BULGE * (0.3f + 0.7f * (1.0f - q));

    float yl = mirror ? y_in : y_out;
    float yr = mirror ? y_out : y_in;

    // 瞳孔：偏内侧、贴住上睑，顶部被切掉
    float pcx = (float)cx + (mirror ? -6.0f : 6.0f) + lx;
    float tp  = eye_clamp((pcx - ((float)cx - rw)) / (float)EYE_W, 0.0f, 1.0f);
    float pcy = yl + (yr - yl) * tp + bulge * sinf(EYE_PI * tp) + 5.0f + ly;

    float px0 = pcx - EYE_R * 0.5f, px1 = pcx + EYE_R * 0.5f;
    float py0 = pcy - EYE_R * 0.5f, py1 = pcy + EYE_R * 0.5f;
    // 高光压在瞳孔中心偏上，不能放到瞳孔顶部 —— 那里在睑线以上，会被裁掉
    float hx0 = px0 + 3.0f, hx1 = hx0 + EYE_HI;
    float hy0 = pcy - 5.0f, hy1 = hy0 + EYE_HI;

    for (int x = (int)floorf((float)cx - rw) - 1; x <= (int)ceilf((float)cx + rw) + 1; x++) {
        float dx = ((float)x + 0.5f - (float)cx) / rw;
        if (dx <= -1.0f || dx >= 1.0f) continue;

        float half_h = rh * sqrtf(1.0f - dx * dx);   // 该列的椭圆半高
        float top = (float)cy - half_h;
        float bot = (float)cy + half_h;

        // 上睑线在该列的高度，取和椭圆边界的较下者
        float t   = eye_clamp(((float)x + 0.5f - ((float)cx - rw)) / (float)EYE_W, 0.0f, 1.0f);
        float lid = yl + (yr - yl) * t + bulge * sinf(EYE_PI * t);
        if (lid > top) top = lid;
        if (bot <= top) continue;

        // 底部灰带（眼球厚度）。可见高度太小的时候不画，否则闭眼那条缝会
        // 整条变成灰的，看着像屏幕坏了。
        float vis_h     = bot - top;
        float grey_h    = vis_h > 14.0f ? eye_min(vis_h * 0.22f, 8.0f) : 0.0f;
        float grey_from = bot - grey_h;

        for (int y = (int)floorf(top); y <= (int)floorf(bot); y++) {
            float a = eye_cover(y, top, bot);
            if (a <= 0.0f) continue;

            float base = ((float)y + 0.5f) > grey_from ? 198.0f : 255.0f;
            float v    = base * a;                        // 黑底上叠白

            // 瞳孔：同样被眼白区域裁剪，所以闭眼时它自然消失
            float ax = eye_min((float)x + 1.0f, px1) - eye_max((float)x, px0);
            float ay = eye_min((float)y + 1.0f, py1) - eye_max((float)y, py0);
            if (ax > 0.0f && ay > 0.0f) {
                float ap = eye_min(ax, 1.0f) * eye_min(ay, 1.0f) * a;
                v += (30.0f - v) * ap;
            }

            float bx = eye_min((float)x + 1.0f, hx1) - eye_max((float)x, hx0);
            float by = eye_min((float)y + 1.0f, hy1) - eye_max((float)y, hy0);
            if (bx > 0.0f && by > 0.0f) {
                float ah = eye_min(bx, 1.0f) * eye_min(by, 1.0f) * a;
                v += (255.0f - v) * ah;
            }

            eye_put(x, y, (int)(v + 0.5f));
        }
    }
}

// 整体呼吸：周期 4.2 秒、振幅 1.2px，慢到几乎察觉不到，但停掉会觉得僵
static int eye_breathe(uint32_t ms)
{
    return (int)lroundf(1.2f * sinf((float)ms / 4200.0f * 2.0f * EYE_PI));
}

static void eyes_render(void)
{
    if (!s_eye_buf) return;

    memset(s_eye_buf, 0, (size_t)EYES_BW * EYES_BH * 2);

    int cy = (EYE_CY - EYES_BY) + eye_breathe(s_eyes.ms);
    int xl = EYE_CX_L - EYES_BX;
    int xr = EYE_CX_R - EYES_BX;

    // 顺序不能反：先眉后眼，眼白才会覆盖掉可能重叠的眉毛像素
    eye_draw_brow(xl, cy, false);
    eye_draw_brow(xr, cy, true);
    eye_draw_eye(xl, cy, s_eyes.open_pct, s_eyes.look_x, s_eyes.look_y, false);
    eye_draw_eye(xr, cy, s_eyes.open_pct, s_eyes.look_x, s_eyes.look_y, true);

    lv_obj_invalidate(s_cv_eyes);
}

static uint32_t eyes_blink_interval(void)
{
    // 困倦时眨眼间隔拉到 7-12 秒，平时 2.4-5.2 秒
    return s_eyes.doze ? (7000 + esp_random() % 5000)
                       : (2400 + esp_random() % 2800);
}

static void eyes_reset(void)
{
    s_eye_last_ms     = (uint32_t)(esp_timer_get_time() / 1000);   // 第一帧 dt 干净
    s_eyes.open_pct   = 0.0f;      // 从合着的状态睁眼，像被叫醒
    s_eyes.base_pct   = 100.0f;
    s_eyes.phase_from = 0.0f;
    s_eyes.phase      = EYES_OPENING;
    s_eyes.phase_ms   = 0;
    s_eyes.look_x = 2.0f;   s_eyes.look_y = -1.0f;
    s_eyes.look_tx = 2.0f;  s_eyes.look_ty = -1.0f;
    s_eyes.look_in_ms  = 1400;
    s_eyes.blink_in_ms = 2400;
    s_eyes.doze = false;
    s_eyes.awake_ms    = 0;
    s_eyes.nod_in_ms   = 45000;
    s_eyes.nod_left_ms = 0;
    s_eyes.ms          = 0;
    // 置成不可能的值，保证第一帧一定重画
    s_eyes.last_open    = -999.0f;
    s_eyes.last_lx      = -999.0f;
    s_eyes.last_ly      = -999.0f;
    s_eyes.last_breathe = -999;
}

static void eyes_tick_cb(lv_timer_t *t)
{
    (void)t;

    /* 用**真实经过的时间**推进状态机，不能用固定的 EYES_MS。
     *
     * lv_timer 只是"最早该在什么时候跑"，实际执行时刻取决于
     * lv_timer_handler 被调用的频率，而那个频率由省电档位决定：
     *   ACTIVE / LIGHT  主循环最多等 50 / 100ms  → 帧率基本准
     *   DEEP            最多等 500ms            → 10 倍慢
     * 写死 EYES_MS 的话，DEEP 档下一次眨眼会拖成 2 秒的慢动作幻灯片。
     * 按真实间隔推进，动画时长就与档位无关，只是帧数少了（更快、更跳）。
     *
     * 上限夹在 EYE_SLIDE_MAX_MS：从被长时间阻塞中恢复时（比如刚
     * wb_eyes_enter），一帧不该把整段眨眼一次性跳完 —— 那样看起来像是
     * "眼睛突然闭了一下"，不如老老实实从当前状态继续。
     * eyes_reset() 会把时间戳归零，所以进入待机屏的第一帧是干净的。 */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t dt  = now - s_eye_last_ms;
    s_eye_last_ms = now;
    if (dt == 0 || dt > EYE_SLIDE_MAX_MS) dt = EYES_MS;

    s_eyes.ms += dt;

    // ---- 犯困：待机满 3 分钟，睑线降到 44%，偶尔点一下头 ----
    if (!s_eyes.doze) {
        s_eyes.awake_ms += dt;
        if (s_eyes.awake_ms >= DOZE_AFTER_MS) {
            s_eyes.doze = true;
            s_eyes.base_pct = 44.0f;
            s_eyes.nod_in_ms = 45000;
            ESP_LOGI(TAG, "眼睛：开始犯困（睑线 44%%）");
        }
    }
    if (s_eyes.nod_left_ms > 0) {
        s_eyes.nod_left_ms = dt < s_eyes.nod_left_ms ? s_eyes.nod_left_ms - dt : 0;
    } else if (s_eyes.doze) {
        s_eyes.nod_in_ms = dt < s_eyes.nod_in_ms ? s_eyes.nod_in_ms - dt : 0;
        if (s_eyes.nod_in_ms == 0) {
            s_eyes.nod_in_ms   = 40000 + esp_random() % 15000;
            s_eyes.nod_left_ms = 900;
        }
    }

    // ---- 眨眼状态机 ----
    switch (s_eyes.phase) {
    case EYES_IDLE:
        s_eyes.blink_in_ms = dt < s_eyes.blink_in_ms ? s_eyes.blink_in_ms - dt : 0;
        if (s_eyes.blink_in_ms == 0) {
            s_eyes.phase      = EYES_CLOSING;
            s_eyes.phase_ms   = 0;
            s_eyes.phase_from = s_eyes.open_pct;
        }
        break;

    case EYES_CLOSING: {
        s_eyes.phase_ms += dt;
        float p = eye_min((float)s_eyes.phase_ms / EYE_CLOSE_MS, 1.0f);
        s_eyes.open_pct = eye_lerp(s_eyes.phase_from, 0.0f, p * p);   // ease-in
        if (p >= 1.0f) { s_eyes.phase = EYES_HOLD; s_eyes.phase_ms = 0; }
        break;
    }

    case EYES_HOLD:
        s_eyes.phase_ms += dt;
        s_eyes.open_pct = 0.0f;
        if (s_eyes.phase_ms >= EYE_HOLD_MS) {
            s_eyes.phase = EYES_OPENING;
            s_eyes.phase_ms = 0;
        }
        break;

    default: {   // EYES_OPENING
        s_eyes.phase_ms += dt;
        float p = eye_min((float)s_eyes.phase_ms / EYE_OPEN_MS, 1.0f);
        s_eyes.open_pct = eye_lerp(0.0f, s_eyes.base_pct, 1.0f - (1.0f - p) * (1.0f - p));
        if (p >= 1.0f) {
            s_eyes.phase    = EYES_IDLE;
            s_eyes.phase_ms = 0;
            // 12% 概率紧接着再眨一下（连眨），比每次都等满间隔更像活物
            s_eyes.blink_in_ms = (esp_random() % 100 < 12) ? 160 : eyes_blink_interval();
        }
        break;
    }
    }

    // 空闲态向"目标开合"缓动。眨眼状态机在跑的时候由它自己说了算。
    if (s_eyes.phase == EYES_IDLE) {
        float target;
        if (s_eyes.nod_left_ms > 0)   target = eye_min(s_eyes.open_pct, 10.0f);
        else if (s_eyes.doze)         target = s_eyes.base_pct;
        else                          target = 100.0f;
        float k = eye_min((float)dt / 260.0f, 1.0f);
        s_eyes.open_pct += (target - s_eyes.open_pct) * k;
    }

    // ---- 视线漂移 ----
    s_eyes.look_in_ms = dt < s_eyes.look_in_ms ? s_eyes.look_in_ms - dt : 0;
    if (s_eyes.look_in_ms == 0) {
        static const int8_t k_pick[8][2] = {
            {0, 0}, {0, 0}, {7, 0}, {-7, 0}, {0, -4}, {0, 2}, {5, -3}, {-5, 2}
        };
        s_eyes.look_in_ms = s_eyes.doze ? (3000 + esp_random() % 4000)
                                        : (1400 + esp_random() % 2200);
        int i = (int)(esp_random() % 8);
        float scale = s_eyes.doze ? 0.45f : 1.0f;   // 困了就不怎么东张西望
        s_eyes.look_tx = k_pick[i][0] * scale;
        s_eyes.look_ty = k_pick[i][1] * scale;
    }
    float lk = eye_min((float)dt / 120.0f, 1.0f);
    s_eyes.look_x += (s_eyes.look_tx - s_eyes.look_x) * lk;
    s_eyes.look_y += (s_eyes.look_ty - s_eyes.look_y) * lk;

    // ---- 只有真变了才重画 ----
    int breathe = eye_breathe(s_eyes.ms);
    bool changed = fabsf(s_eyes.open_pct - s_eyes.last_open) > 0.8f
                || fabsf(s_eyes.look_x   - s_eyes.last_lx)   > 0.4f
                || fabsf(s_eyes.look_y   - s_eyes.last_ly)   > 0.4f
                || breathe != s_eyes.last_breathe;
    if (!changed) return;

    s_eyes.last_open    = s_eyes.open_pct;
    s_eyes.last_lx      = s_eyes.look_x;
    s_eyes.last_ly      = s_eyes.look_y;
    s_eyes.last_breathe = breathe;
    eyes_render();
}

bool wb_eyes_build(lv_obj_t *parent)
{
    if (s_cv_eyes) return true;          // 已建好，幂等
    const size_t bytes = (size_t)EYES_BW * EYES_BH * 2;

    s_eye_buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_eye_buf) {
        // 没 PSRAM 就退化成纯文字待机页。为了眼睛把整机拖垮不值得。
        ESP_LOGE(TAG, "眼睛画布分配失败（%u 字节），待机页退化为纯文字",
                 (unsigned)bytes);
        return false;
    }
    memset(s_eye_buf, 0, bytes);

    s_cv_eyes = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_cv_eyes, s_eye_buf, EYES_BW, EYES_BH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_cv_eyes, EYES_BX, EYES_BY);

    s_eye_timer = lv_timer_create(eyes_tick_cb, EYES_MS, NULL);
    if (!s_eye_timer) {
        ESP_LOGE(TAG, "眼睛定时器创建失败，动作不会动");
        return false;
    }
    lv_timer_pause(s_eye_timer);        // 只在待机页跑，醒着时一帧都不算

    ESP_LOGI(TAG, "眼睛画布: %dx%d RGB565, %u KB (PSRAM)",
             EYES_BW, EYES_BH, (unsigned)(bytes / 1024));
    return true;
}

bool wb_eyes_ready(void) { return s_eye_timer != NULL; }

void wb_eyes_enter(void)
{
    if (!s_eye_timer) return;
    eyes_reset();
    lv_timer_resume(s_eye_timer);
}

void wb_eyes_leave(void)
{
    if (s_eye_timer) lv_timer_pause(s_eye_timer);
}

void wb_eyes_free(void)
{
    /* 顺序：先停并删定时器，再删画布，最后释放缓冲。
     *
     * 定时器必须先停：它每 50ms 调 eyes_render()，那里会解引用
     * s_eye_buf 和 s_cv_eyes。先删画布再删定时器的话，中间若插进一帧
     * lv_timer_handler，就是一次野指针写入。
     *
     * 注意 s_eye_buf 是 lv_canvas_set_buffer 的**外部缓冲**，LVGL 不会
     * 自己释放，必须手动 heap_caps_free。 */
    if (s_eye_timer) {
        lv_timer_delete(s_eye_timer);
        s_eye_timer = NULL;
    }
    if (s_cv_eyes) {
        lv_obj_delete(s_cv_eyes);
        s_cv_eyes = NULL;
    }
    if (s_eye_buf) {
        heap_caps_free(s_eye_buf);
        s_eye_buf = NULL;
    }
    // 状态置成"没建过"，下次 build 从头来
    memset(&s_eyes, 0, sizeof(s_eyes));
    s_eye_last_ms = 0;
}
