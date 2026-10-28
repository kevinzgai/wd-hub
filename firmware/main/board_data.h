/* 看板数据模型 + 共享快照
 *
 * 从 main.cpp 拆出来：字段上限、结构体、线程安全的快照读写都在这里。
 * JSON 解析在 board_json.cpp，渲染和取数各自只 include 这个头。
 *
 * 为什么把快照读写也收进来：原来 s_data + s_data_lock 直接暴露在 main.cpp，
 * 取数任务和渲染任务各自手写 xSemaphoreTake / Give。收进来之后读写只有
 * 一对函数，"谁在碰这份数据"一眼能看完；以后加字段也不用回主文件找加锁点。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ 字段上限 */

#define MAX_SESSIONS 3
#define TEXT_TITLE   40
/* 实时动作名（"跑命令"/"等模型"/"等你回答"）。
 * 服务端已翻译成中文，固件不维护映射表。 */
#define TEXT_ACTION  12
#define TEXT_PROJECT 20
#define TEXT_ELAPSED 12

/* 详情屏：服务端下发"标签 + 右值"的行，固件只负责画，不认识业务语义。
 * 想改每屏显示什么只改服务端 _screen_rows()，固件不用动。 */
#define MAX_ROWS     6
#define TEXT_ROW     40        // 行主文本，16px 字体约 13 个中文
#define TEXT_ROWVAL  16        // 右侧数值，12px 字体

/* 图表屏：服务端下发 kind="chart" + charts 数组，固件画成环形仪表。
 * 3 个是屏幕宽度决定的 —— 62px 环 × 3 + 间距 11 × 2 = 208，刚好落在 224 的
 * 内容区里。想加第四个环只能缩到 46px，内径装不下 "100%"。 */
#define MAX_CHARTS   3
/* 屏号上限，只为 s_page_is_chart[] 定长。服务端 nscr 目前 5，
 * 留一倍余量，真加到 8 屏也不用改这里。 */
#define MAX_PAGES    8

/* -------------------------------------------------------------------- 结构体 */

typedef struct {
    char    t[TEXT_ROW];
    char    v[TEXT_ROWVAL];
    uint8_t color;             // 0 普通 / 1 强调蓝 / 2 成功绿 / 3 警告黄 / 4 暗灰
} row_t;

typedef struct {
    char    k[8];              // 指标键 cpu/mem/disk，仅用于日志和调试
    char    n[12];             // 环下名称（服务端给中文，固件不认语义）
    int     v;                 // 0..100
    uint8_t color;             // 1 蓝 / 2 绿 / 3 黄 / 5 红
} chart_t;

typedef struct {
    char    title[TEXT_TITLE];
    char    project[TEXT_PROJECT];
    char    elapsed[TEXT_ELAPSED];  // 服务端算好的成品串，e 缺失时的回退
    int     age_s;          // 服务端拍下这一行时，距最近活动多少秒
    uint8_t waiting;        // 1 = 等待操作，行首黄灯闪
    /* ---- 实时状态（2026-10-28 新增）----
     * st: 0 空闲 / 1 运行中（绿灯）/ 2 等你确认（黄灯闪）
     * act: 当前动作的中文名，空字符串表示服务端没给
     *
     * 这两个键是**可选**的：服务端不发时 st=0、act 空，
     * 行为跟旧固件完全一致。新固件读老服务端也不会崩。 */
    uint8_t state;         // 见上：0空闲 1运行 2等确认
    char    action[TEXT_ACTION];  // 当前动作（中文）
} session_t;

typedef struct {
    bool      ok;
    char      error[64];
    char      clock[8];
    char      date[8];
    // 数据有多新（秒），服务端 /status 与 /screen 都下发。
    // -1 = 服务端没给（旧版服务或取不到采集时间），页脚显示 "--"。
    // 这不是 clock 那种"服务端此刻几点"，而是生成的快照距现在多久 ——
    // 只有这个数才能回答"屏幕上的数是不是新鲜的"。
    int       age;
    int       working;
    int       today_done;
    int       automations;
    int       cpu;          // %，-1 = 服务端没给该字段（旧版服务）
    int       mem;          // %
    int       disk;         // %
    int       temp;         // °C，0 = 取不到（Windows 上通常如此）
    // 芯片结温（°C），服务端从 /device/report 透传下来，-1 = 设备还没报过。
    // 这是**本机**的温度传感器读数，不是 PC 的 —— 和上面 temp 完全两回事。
    int       chip_temp;
    session_t sessions[MAX_SESSIONS];
    int       session_count;
    // ---- 详情屏 ----
    char      page_title[24];
    row_t     rows[MAX_ROWS];
    int       row_count;
    // ---- 图表屏（服务端 kind="chart" 时才非空）----
    // rows 与 charts 并存：旧服务端只发 rows，新服务端两者都发。
    // 固件优先 charts，charts 为空就退回文本行，两边都能显示。
    chart_t   charts[MAX_CHARTS];
    int       chart_count;
    bool      is_chart;
    char      foot[2][24];  // 图表屏底部两行成品文本（进程 / 运行时长）
    int       foot_count;
    int       nscr;         // 服务端下发的屏数（含概览屏）
    int       page;         // 这批数据属于第几屏，用于丢弃"切页前发出的请求"的回包
    // 本地单调时钟在这批数据被解析时的读数（ms）。会话计时靠它做锚点：
    // 屏幕上的"多久之前" = age_s + (现在 - sample_ms)/1000，所以秒针能一秒一跳，
    // 不受 5 秒轮询节奏约束。拉取失败沿用旧数据时这个值原样保留，计时继续走。
    int64_t   sample_ms;
} board_data_t;

/* ------------------------------------------------------------------ 快照读写 */

/* 建互斥量。必须在任何 reader/writer 任务启动前调用一次。 */
void board_data_init(void);

/* 取一份快照副本。返回 false = 200ms 内没拿到锁（out 保持不变），
 * 调用方跳过这一帧即可 —— 绘制走 SPI 可能几十毫秒，不该让 fetch 干等。 */
bool board_data_get(board_data_t *out);

/* 整份替换快照。返回 false = 没拿到锁，这轮数据被丢弃（下轮还有）。 */
bool board_data_set(const board_data_t *in);

/* 屏数。服务端每轮都下发，读写都走这两个函数；int 读写本身是原子的，
 * 不需要锁 —— 渲染那边只是拿它算环形切换的模。 */
int  board_data_nscr(void);
void board_data_set_nscr(int n);
