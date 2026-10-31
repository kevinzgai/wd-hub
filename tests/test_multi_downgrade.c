/* 固件侧多选降级测试。
 *
 * 背景（2026-10-31 实测发现）：设备只有一颗键，短按/长按分别是
 * "移动光标/提交"，没有第三种手势去勾选 —— 固件因此**不实现**多选勾选
 * （wb_view.c 也不画 [ ]/[x] 勾选框）。
 *
 * 隐患：主机若硬发 multi=1，设备会进多选分支，但 mask 恒为 0，
 * submit_ask 以"多选未勾选"拒绝提交 → 用户停在提问页怎么按都没反应。
 *
 * 修复：wb_core.c 在收 ASK 帧时按 WB_CAP_ASK_MULTI 归零（未实现时恒 0），
 * 让多选降级为单选。本测试验证该降级确实生效。
 */
#include <stdio.h>
#include <string.h>
#include "wb_proto.h"
#include "wb_core.h"

static int fails = 0;
#define CK(c, m) do { if (!(c)) { printf("  ✗ %s\n", m); fails++; } \
                     else printf("  ✓ %s\n", m); } while (0)

/* 主机下发多选题（multi=1）——模拟主机未做能力协商的旧行为 */
static const char *ASK_MULTI =
    "{\"type\":\"ask\",\"id\":9202,\"tool\":\"AskUserQuestion\","
    "\"text\":\"要跑哪几种测试？\","
    "\"opts\":[\"单元测试\",\"集成测试\",\"E2E 测试\",\"性能测试\"],"
    "\"ans\":1,\"multi\":1}";

/* 主机下发单选题 */
static const char *ASK_SINGLE =
    "{\"type\":\"ask\",\"id\":9101,\"tool\":\"AskUserQuestion\","
    "\"text\":\"选哪个环境部署？\","
    "\"opts\":[\"staging 测试环境\",\"production 生产环境\","
    "\"本地 docker\",\"跳过，先不改\"],\"ans\":1,\"multi\":0}";

int main(void)
{
    wb_ev_t ev;
    wb_state_t s;
    wb_tick_t t;

    printf("WB_CAP_ASK_MULTI = 0x%x  WB_CAPS = 0x%x（本固件%s实现多选勾选）\n",
           WB_CAP_ASK_MULTI, WB_CAPS,
           (WB_CAPS & WB_CAP_ASK_MULTI) ? "已" : "未");

    puts("\n[1] 多选题降级为单选");
    {
        memset(&s, 0, sizeof(s));
        memset(&ev, 0, sizeof(ev));
        CK(wb_proto_decode(ASK_MULTI, strlen(ASK_MULTI), &ev), "帧解码成功");
        CK(ev.ask_multi == 1, "协议层确实解出了 multi=1");
        CK(ev.n_opts == 4, "选项数 4");

        wb_core_init(&s, 0);
        (void)wb_core_apply(&s, &ev, 0, NULL);
        CK(s.ask.active, "提问页已激活");
        CK(s.ask.multi == 0,
           "**核心层已把 multi 降为 0**（否则用户会卡在死页面）");
        CK(s.ask.mask == 0, "mask 为 0（无勾选能力）");
        CK(s.ask.n_opts == 4, "选项仍然保留 4 项");
    }

    puts("\n[2] 单选题不受影响");
    {
        memset(&s, 0, sizeof(s));
        memset(&ev, 0, sizeof(ev));
        CK(wb_proto_decode(ASK_SINGLE, strlen(ASK_SINGLE), &ev), "帧解码成功");
        wb_core_init(&s, 0);
        (void)wb_core_apply(&s, &ev, 0, NULL);
        CK(s.ask.active, "提问页已激活");
        CK(s.ask.multi == 0, "multi 仍为 0");
        CK(s.ask.cursor == 0, "游标从 0 开始");
    }

    puts("\n[3] 降级后提交能正常编码出 index（而非 mask）");
    {
        /* 单选提交路径：enc_ask_index 应产出可解析的 JSON */
        char up[128];
        const size_t n = wb_proto_enc_ask_index(up, sizeof(up), 9202, 2);
        CK(n > 0, "编码出上行帧");
        CK(strstr(up, "\"type\":\"ask_answer\"") != NULL, "帧类型 ask_answer");
        CK(strstr(up, "\"id\":9202") != NULL, "带着原提问 id");
        CK(strstr(up, "\"index\":2") != NULL, "回传的是 index（单选语义）");
        CK(strstr(up, "mask") == NULL, "不含 mask 字段");
    }

    puts("\n[4] 多选 mask 编码路径仍可用（将来实现勾选后直接复用）");
    {
        char up[128];
        const size_t n = wb_proto_enc_ask_mask(up, sizeof(up), 9202, 0x5);
        CK(n > 0, "编码出上行帧");
        CK(strstr(up, "\"mask\":5") != NULL, "mask 位图正确");
    }

    puts("\n[5] 重复下发同一 multi 题（保活重推）也保持降级");
    {
        memset(&s, 0, sizeof(s));
        memset(&ev, 0, sizeof(ev));
        (void)wb_proto_decode(ASK_MULTI, strlen(ASK_MULTI), &ev);
        wb_core_init(&s, 0);
        (void)wb_core_apply(&s, &ev, 0, NULL);
        const int cursor_before = s.ask.cursor;
        /* 保活重推：同 id 的 ask 再来一次 */
        memset(&ev, 0, sizeof(ev));
        (void)wb_proto_decode(ASK_MULTI, strlen(ASK_MULTI), &ev);
        (void)wb_core_apply(&s, &ev, 0, NULL);
        CK(s.ask.multi == 0, "保活重推后 multi 仍为 0");
        CK(s.ask.cursor == cursor_before, "保活重推未重置游标");
    }

    printf("\n%s\n", fails ? "存在失败项" : "全部通过");
    return fails ? 1 : 0;
}
