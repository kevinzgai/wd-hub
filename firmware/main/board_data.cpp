/* 看板数据模型 + 共享快照 —— 实现
 *
 * 只有一份 s_snap，写者唯一（fetch 任务），读者是渲染任务。
 * 锁的粒度是"整份拷贝"：POD 结构几百字节，memcpy 是微秒级，
 * 比让 fetch 等渲染画完（可能几十毫秒）划算得多。
 */
#include "board_data.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 渲染侧等锁上限：50ms 是原 main.cpp 里的取值 ——
 * 一帧最多等这么久，超了就跳过这帧（下一帧 20~50ms 后就来）。 */
#define BOARD_DATA_GET_TIMEOUT_MS  50
/* fetch 侧等锁上限：宽一点，这一轮数据是刚拉回来的，丢掉可惜。 */
#define BOARD_DATA_SET_TIMEOUT_MS  1000

static board_data_t      s_snap;
static SemaphoreHandle_t s_lock;
static int               s_nscr = 1;   // 屏数，由服务端下发

void board_data_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool board_data_get(board_data_t *out)
{
    if (!out) return false;

    /* 还没有锁（任务已经起来但 init 没跑）时不要直接读 ——
     * 那样等于绕过保护。返回 false 让调用方跳过这一帧。 */
    if (!s_lock) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_DATA_GET_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    *out = s_snap;
    xSemaphoreGive(s_lock);
    return true;
}

bool board_data_set(const board_data_t *in)
{
    if (!in || !s_lock) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_DATA_SET_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    s_snap = *in;
    xSemaphoreGive(s_lock);
    return true;
}

int board_data_nscr(void)
{
    return s_nscr;
}

void board_data_set_nscr(int n)
{
    if (n > 0) s_nscr = n;
}
