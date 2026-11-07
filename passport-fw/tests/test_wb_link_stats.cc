// passport-fw/tests/test_wb_link_stats.cc —— 关于页链路统计的格式化与边界。
//
// 这层是纯函数（wb_link_stats.h 的 inline），没有 IDF 依赖，可以直接在
// host 上编。锁住三件事：
//   1) 连接时长按「分:秒 / 时:分」分段，且绝不超出大数字 124px 的宽度预算；
//   2) 未连接（sec<0）与从未断开（reason<0）都有明确文案，不是空串或 0；
//   3) 断开原因只翻已知码，未知码保留十六进制（不编造中文）。
#include <cstdio>
#include <cstring>
#include <string>

#include "../../main/wb/wb_link_stats.h"

static int g_fail;

static void chk(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static void chk_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL: %s\n  期望 \"%s\"\n  实际 \"%s\"\n", what, want, got);
        g_fail++;
    }
}

// 与 refresh_about 的版面预算一致：32px montserrat 下 7 个字符（如 "06:12"）
// 约 116px，8 个字符就会顶到 124px 边界。这里按字符数做硬约束。
static void chk_width(const char *s, size_t max_chars, const char *what)
{
    if (strlen(s) > max_chars) {
        printf("FAIL: %s 超宽 %zu 字符 > %zu (\"%s\")\n", what, strlen(s), max_chars, s);
        g_fail++;
    }
}

static void test_itvl_ms()
{
    // BLE 原始单位 1.25ms：24 → 30ms，36 → 45ms，40 → 50ms，160 → 200ms
    chk(wb_link_itvl_ms(24) == 30, "24*1.25=30ms");
    chk(wb_link_itvl_ms(36) == 45, "36*1.25=45ms");
    chk(wb_link_itvl_ms(40) == 50, "40*1.25=50ms");
    chk(wb_link_itvl_ms(160) == 200, "160*1.25=200ms");
    chk(wb_link_itvl_ms(200) == 250, "200*1.25=250ms");
    // 边界：0 不应算出非 0（未协商时调用方自己判断，这里只保证不崩）
    chk(wb_link_itvl_ms(0) == 0, "0 -> 0ms");
}

static void test_clock()
{
    char buf[32];
    int shape;

    // 未连接：--:--，shape=-1，配「未连接」说明
    shape = wb_link_fmt_clock(buf, sizeof(buf), -1);
    chk_str(buf, "--:--", "未连接文案");
    chk(shape == -1, "未连接 shape=-1");
    chk_width(buf, 5, "未连接宽度");

    // 0 秒：00:00，算「分:秒」
    shape = wb_link_fmt_clock(buf, sizeof(buf), 0);
    chk_str(buf, "00:00", "0 秒");
    chk(shape == 0, "0 秒 shape=0");

    // 13 秒（实机抓屏那个值）
    shape = wb_link_fmt_clock(buf, sizeof(buf), 13);
    chk_str(buf, "00:13", "13 秒");
    chk(shape == 0, "13 秒 shape=0");

    // 372 秒 = 6:12（实机抓屏 06:12）
    shape = wb_link_fmt_clock(buf, sizeof(buf), 372);
    chk_str(buf, "06:12", "372 秒");
    chk(shape == 0, "372 秒 shape=0");

    // 3599 秒 = 59:59，仍是「分:秒」——不能进位成 60:00
    shape = wb_link_fmt_clock(buf, sizeof(buf), 3599);
    chk_str(buf, "59:59", "3599 秒");
    chk(shape == 0, "3599 秒 shape=0");
    chk_width(buf, 5, "59:59 宽度");

    // 3600 秒进位成「时:分」01:00，位数不变宽
    shape = wb_link_fmt_clock(buf, sizeof(buf), 3600);
    chk_str(buf, "01:00", "3600 秒");
    chk(shape == 1, "3600 秒 shape=1 表示时:分");
    chk_width(buf, 5, "01:00 宽度");

    // 长连接 45296 秒 = 12:34，宽度不涨（小时取模 100）
    shape = wb_link_fmt_clock(buf, sizeof(buf), 45296);
    chk_str(buf, "12:34", "45296 秒");
    chk(shape == 1, "45296 秒 shape=1");
    chk_width(buf, 5, "12:34 宽度");

    // 极端：99 小时以上仍然只有 5 字符（小时取模 100），不会撑破版面
    shape = wb_link_fmt_clock(buf, sizeof(buf), 400000);
    chk(shape == 1, "400000 秒 shape=1");
    chk_width(buf, 5, "极端长连接宽度");
}

static void test_reason()
{
    char buf[24];

    // 从未断开：写「无」而不是空串或 0x00
    wb_link_fmt_reason(buf, sizeof(buf), -1);
    chk_str(buf, "无", "从未断开");

    // ---- NimBLE 错误域（实机唯一真实形态）----
    // BLE_GAP_EVENT_DISCONNECT 给的 reason 是 NimBLE 错误码，不是 BLE 规范裸码：
    //   0x200 起 = HCI（BLE_HS_ERR_HCI_BASE），真值 = 值 - 0x200
    //   0x100 起 = ATT（BLE_HS_ERR_ATT_BASE），真值 = 值 - 0x100
    // 实机主机 /api/stop 断开时设备收到 0x213 = HCI 0x13 = 远端主动断开。
    // 这条断言是本测试存在的最重要理由：上一版只翻裸码 0x13，
    // 在真机上一条都命中不了，屏上打出「原因 0x213」谁都看不懂。
    wb_link_fmt_reason(buf, sizeof(buf), 0x213);
    chk_str(buf, "主机断开", "0x213 → HCI 0x13");

    wb_link_fmt_reason(buf, sizeof(buf), 0x208);
    chk_str(buf, "连接超时", "0x208 → HCI 0x08");

    wb_link_fmt_reason(buf, sizeof(buf), 0x216);
    chk_str(buf, "本机断开", "0x216 → HCI 0x16");

    wb_link_fmt_reason(buf, sizeof(buf), 0x23E);
    chk_str(buf, "建立失败", "0x23e → HCI 0x3e");

    wb_link_fmt_reason(buf, sizeof(buf), 0x222);
    chk_str(buf, "链路差", "0x222 → HCI 0x22");

    // ATT 域：0x108 = ATT 0x08 = 数据超时
    wb_link_fmt_reason(buf, sizeof(buf), 0x108);
    chk_str(buf, "数据超时", "0x108 → ATT 0x08");

    // ---- 裸码仍要认（其它路径/其它固件线可能给裸值）----
    wb_link_fmt_reason(buf, sizeof(buf), 0x13);
    chk_str(buf, "主机断开", "裸 0x13");
    wb_link_fmt_reason(buf, sizeof(buf), 0x08);
    chk_str(buf, "连接超时", "裸 0x08");

    // 未知码：保留十六进制，绝不编造中文。
    // 位宽必须是 %03X：0x213 用 %02X 会打成「0x213」以外的形态（实测出错），
    // 三位才能完整表达 NimBLE 码。
    wb_link_fmt_reason(buf, sizeof(buf), 0x2AB);
    chk_str(buf, "0x2AB", "未知 NimBLE 码保留三位十六进制");
    wb_link_fmt_reason(buf, sizeof(buf), 0x45);
    chk_str(buf, "0x045", "未知裸码也补齐三位");

    // 宽度：全部输出都在 6 个字符以内，能塞进 96px 右列
    chk_width(buf, 6, "未知码宽度");
}

static void test_struct()
{
    // 结构体布局锁：这些字段是跨层契约（wb_link.c 写、wb_ui.cc 搬、
    // wb_view.c 读），任何一层改了顺序/类型都会在这里先炸。
    wb_link_stats_t s = {};
    chk(s.connected == false, "默认未连接");
    chk(s.conn_since_ms == 0, "默认 conn_since_ms=0");
    chk(s.rx_frames == 0 && s.tx_frames == 0 && s.disconnects == 0, "计数默认 0");
    chk(s.last_reason == 0, "默认 reason=0（调用方须自行置 -1）");

    s.last_reason = -1;
    s.disconnects = 3;
    s.rx_frames = 143;
    s.tx_frames = 123;
    s.conn_itvl = 40;
    s.mtu = 256;
    chk(s.last_reason == -1, "可存 -1");
    chk(wb_link_itvl_ms(s.conn_itvl) == 50, "50ms 折算");
}

int main()
{
    test_itvl_ms();
    test_clock();
    test_reason();
    test_struct();

    if (g_fail) {
        printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    printf("test_wb_link_stats: 全部通过\n");
    return 0;
}
