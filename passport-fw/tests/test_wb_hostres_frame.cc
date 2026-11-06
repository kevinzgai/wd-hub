// passport-fw/tests/test_wb_hostres_frame.cc —— host 帧「电脑资源」字段解码。
//
// 覆盖三种主机形态，这三种在线上都会出现：
//   1) 新主机：网关信息 + CPU/内存/磁盘全发；
//   2) 老主机：只发网关信息，一个资源键都没有 → 必须落成 -1，
//      否则屏幕会把「没数据」画成「占用 0%」；
//   3) 半新主机：只发了占用率没发绝对值（或反之）→ 缺的那几个独立落 -1。
// 另外验证越界值被钳制：主机采集异常给出 130% 或 -5 时不能画出越界条。
#include <cassert>
#include <cstdio>
#include <cstring>

#include "wb_proto.h"

namespace {

wb_ev_t decode(const char *json)
{
    wb_ev_t ev;
    memset(&ev, 0, sizeof(ev));
    const bool ok = wb_proto_decode(json, strlen(json), &ev);
    assert(ok);
    assert(ev.type == WB_EV_HOST);
    return ev;
}

// 老主机：host 帧只有网关四件套，没有任何资源键。
void test_legacy_host_frame()
{
    const char *frame = "{\"host\":1,\"gw_ports\":3,\"gw_sessions\":2,"
                        "\"secret\":true,\"cli\":false,\"uptime\":120}";
    const wb_ev_t ev = decode(frame);

    assert(ev.gw_ports == 3);
    assert(ev.gw_sessions == 2);
    assert(ev.gw_secret);
    assert(!ev.gw_cli);
    assert(ev.uptime_s == 120);

    // 关键断言：缺键必须是 -1，不能是 0。
    assert(ev.res.cpu_pct == -1);
    assert(ev.res.mem_pct == -1);
    assert(ev.res.mem_used_mb == -1);
    assert(ev.res.mem_total_mb == -1);
    assert(ev.res.disk_pct == -1);
    assert(ev.res.disk_used_mb == -1);
    assert(ev.res.disk_total_mb == -1);
}

// 新主机：全字段。
void test_full_host_frame()
{
    const char *frame = "{\"host\":1,\"gw_ports\":3,\"gw_sessions\":2,"
                        "\"secret\":true,\"cli\":true,\"uptime\":98765,"
                        "\"cpu_pct\":37,\"mem_pct\":68,"
                        "\"mem_used_mb\":10922,\"mem_total_mb\":16384,"
                        "\"disk_pct\":41,"
                        "\"disk_used_mb\":419430,\"disk_total_mb\":1024000}";
    const wb_ev_t ev = decode(frame);

    assert(ev.res.cpu_pct == 37);
    assert(ev.res.mem_pct == 68);
    assert(ev.res.mem_used_mb == 10922);
    assert(ev.res.mem_total_mb == 16384);
    assert(ev.res.disk_pct == 41);
    assert(ev.res.disk_used_mb == 419430);
    assert(ev.res.disk_total_mb == 1024000);
}

// 半新主机：只有占用率，没有绝对值。
void test_percent_only_frame()
{
    const char *frame = "{\"host\":1,\"cpu_pct\":12,\"mem_pct\":45,\"disk_pct\":20}";
    const wb_ev_t ev = decode(frame);

    assert(ev.res.cpu_pct == 12);
    assert(ev.res.mem_pct == 45);
    assert(ev.res.disk_pct == 20);
    // 绝对值缺失 → -1，界面写「总量未知」而不是「0 已用」。
    assert(ev.res.mem_used_mb == -1);
    assert(ev.res.mem_total_mb == -1);
    assert(ev.res.disk_used_mb == -1);
    assert(ev.res.disk_total_mb == -1);
}

// 越界与畸形值必须被钳制，不能把负值/超范围值传到条形控件。
void test_clamping()
{
    const char *frame = "{\"host\":1,\"cpu_pct\":130,\"mem_pct\":-5,"
                        "\"disk_pct\":\"lots\",\"mem_total_mb\":-9}";
    const wb_ev_t ev = decode(frame);

    assert(ev.res.cpu_pct == 100);     // 上限钳到 100
    assert(ev.res.mem_pct == 0);       // 负数钳到 0（占用率不会是负的）
    assert(ev.res.disk_pct == -1);     // 类型不对 → 未下发
    assert(ev.res.mem_total_mb == -1); // 负容量 → 未下发
}

// 资源帧不能影响原有帧类型判定：usage 仍走 USAGE，credit 仍走 CREDIT。
void test_no_type_regression()
{
    wb_ev_t ev;
    memset(&ev, 0, sizeof(ev));
    const char *usage = "{\"usage\":1,\"today\":100,\"req\":2,\"cache\":0}";
    assert(wb_proto_decode(usage, strlen(usage), &ev));
    assert(ev.type == WB_EV_USAGE);

    memset(&ev, 0, sizeof(ev));
    const char *credit = "{\"remain\":742.5,\"total\":1000}";
    assert(wb_proto_decode(credit, strlen(credit), &ev));
    assert(ev.type == WB_EV_CREDIT);
}

}  // namespace

int main()
{
    test_legacy_host_frame();
    test_full_host_frame();
    test_percent_only_frame();
    test_clamping();
    test_no_type_regression();
    std::puts("WB host resources: legacy/full/partial decode, clamping, "
              "no frame-type regression: PASS");
    return 0;
}
