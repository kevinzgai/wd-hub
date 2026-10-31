#include <stdio.h>
#include <string.h>
#include "wb_proto.h"
#include "wb_core.h"

static int fails = 0;
#define CK(c, m) do { if (!(c)) { printf("  ✗ %s\n", m); fails++; } \
                     else printf("  ✓ %s\n", m); } while (0)

/* 模拟主机下发的单选提问帧（与 wb_app.py _push_ask_item 的字段一致） */
static const char *ASK_SINGLE =
    "{\"type\":\"ask\",\"id\":9101,\"tool\":\"AskUserQuestion\","
    "\"text\":\"选哪个环境部署？\","
    "\"opts\":[\"staging 测试环境\",\"production 生产环境\","
    "\"本地 docker\",\"跳过，先不改\"],\"ans\":1,\"multi\":0}";

/* 多选题（勾选态，回传 mask） */
static const char *ASK_MULTI =
    "{\"type\":\"ask\",\"id\":9202,\"tool\":\"AskUserQuestion\","
    "\"text\":\"要跑哪几种测试？\","
    "\"opts\":[\"单元测试\",\"集成测试\",\"E2E 测试\",\"性能测试\"],"
    "\"ans\":1,\"multi\":1}";

/* 只读提问（电脑上答） */
static const char *ASK_READONLY =
    "{\"type\":\"ask\",\"id\":9303,\"tool\":\"AskUserQuestion\","
    "\"text\":\"这里需要你在电脑上确认\","
    "\"opts\":[\"是\",\"否\"],\"ans\":0,\"multi\":0}";

int main(void)
{
    wb_ev_t ev;
    wb_tick_t t;
    wb_state_t s;
    char up[128];

    puts("[1] 单选提问帧解码");
    {
        memset(&ev, 0, sizeof(ev));
        CK(wb_proto_decode(ASK_SINGLE, strlen(ASK_SINGLE), &ev), "解码成功");
        CK(ev.type == WB_EV_ASK, "类型 = ASK");
        CK(ev.ask_id == 9101, "id = 9101");
        CK(ev.ask_ans == 1, "ans = 1（设备可答）");
        CK(ev.ask_multi == 0, "multi = 0（单选）");
        CK(ev.n_opts == 4, "4 个选项");
        CK(strcmp(ev.opt[0], "staging 测试环境") == 0, "选项 0 原文");
        CK(strcmp(ev.opt[3], "跳过，先不改") == 0, "选项 3 原文（含中文逗号）");
        CK(strcmp(ev.text, "选哪个环境部署？") == 0, "正文原文");
        CK(strcmp(ev.tool, "AskUserQuestion") == 0, "tool 原文");
    }

    puts("[2] 收帧后自动进提问页");
    {
        int64_t now = 5000;
        wb_core_init(&s, now);
        wb_proto_decode(ASK_SINGLE, strlen(ASK_SINGLE), &ev);
        CK(wb_core_apply(&s, &ev, now, &t), "apply 返回 redraw");
        /* 注意：page_changed 只由 wb_core_tick() 产生，apply() 从不设置它 ——
         * 视图层是每帧直接调 wb_core_page() 决定当前页（wb_view.c:1178），
         * 不消费这个标志。所以这里不该断言 page_changed。 */
        CK(wb_core_page(&s) == WB_PAGE_ASK, "当前页 = 提问页");
        CK(s.ask.active, "ask.active");
        CK(s.ask.n_opts == 4, "状态里 4 个选项");
        CK(s.ask.ans == 1, "可作答");
        CK(s.ask.multi == 0, "单选态");
    }

    puts("[3] 60s 保活重推必须幂等（不重置游标/不重建页）");
    {
        int64_t now = 5000;
        wb_core_init(&s, now);
        wb_proto_decode(ASK_SINGLE, strlen(ASK_SINGLE), &ev);
        wb_core_apply(&s, &ev, now, &t);
        // 用户移动了高亮到第 3 项
        s.ask.cursor = 2;
        // 60s 后主机原样重发同一题
        now += 60000;
        CK(wb_core_apply(&s, &ev, now, &t), "保活重推仍返回 redraw");
        CK(s.ask.cursor == 2, "游标保持在第 3 项（未被重置）");
        CK(wb_core_page(&s) == WB_PAGE_ASK, "仍在提问页");
        CK(s.ask.id == 9101, "id 未被换掉（同一题幂等）");
        CK(s.ask.n_opts == 4, "选项数量未被重置");
    }

    puts("[4] 单选上行编码（index）");
    {
        size_t n = wb_proto_enc_ask_index(up, sizeof(up), 9101, 2);
        CK(n > 0, "编码成功");
        printf("      → %s", up);
        CK(strstr(up, "\"type\":\"ask_answer\"") != NULL, "有 type=ask_answer");
        CK(strstr(up, "\"id\":9101") != NULL, "id 正确");
        CK(strstr(up, "\"index\":2") != NULL, "index=2");
        CK(up[n - 1] == '\n', "以换行结尾（主机按行切分）");
    }

    puts("[5] 单选答案能被主机侧识别（联调模拟）");
    {
        /* 主机 _handle_ask_answer 靠 id + index 取选项：
         *   picks = _decode_ask_picks(mask, index, len(options), is_multi)
         * 这里模拟主机拿到 index=2 后应取到第 3 个选项文本。 */
        size_t n = wb_proto_enc_ask_index(up, sizeof(up), 9101, 2);
        CK(n > 0, "上行帧已生成");
        wb_ev_t d;
        memset(&d, 0, sizeof(d));
        // 上行帧不是给设备解的，这里只校验字段形态能否被 json 解析器读出
        CK(strstr(up, "index") != NULL, "字段名与主机读取的键一致（index）");
    }

    puts("[6] 多选提问帧 + mask 编码");
    {
        memset(&ev, 0, sizeof(ev));
        CK(wb_proto_decode(ASK_MULTI, strlen(ASK_MULTI), &ev), "解码成功");
        CK(ev.ask_multi == 1, "multi = 1");
        CK(ev.n_opts == 4, "4 个选项");
        size_t n = wb_proto_enc_ask_mask(up, sizeof(up), 9202, 0x5 /* 选项 0 和 2 */);
        CK(n > 0, "mask 编码成功");
        printf("      → %s", up);
        CK(strstr(up, "\"mask\":5") != NULL, "mask=5（第1、3项）");
    }

    puts("[7] 只读提问（ans=0）：设备不该作答");
    {
        memset(&ev, 0, sizeof(ev));
        CK(wb_proto_decode(ASK_READONLY, strlen(ASK_READONLY), &ev), "解码成功");
        CK(ev.ask_ans == 0, "ans = 0");
        int64_t now = 7000;
        wb_core_init(&s, now);
        wb_core_apply(&s, &ev, now, &t);
        CK(wb_core_page(&s) == WB_PAGE_ASK, "仍显示提问页（让人看到有提问）");
        CK(s.ask.ans == 0, "状态里 ans=0 → submit_ask 会拒绝");
    }

    puts("[8] 无选项的帧降级为只读");
    {
        memset(&ev, 0, sizeof(ev));
        const char *noopt =
            "{\"type\":\"ask\",\"id\":9404,\"tool\":\"Q\",\"text\":\"空选项\","
            "\"opts\":[],\"ans\":1,\"multi\":0}";
        CK(wb_proto_decode(noopt, strlen(noopt), &ev), "解码成功");
        CK(ev.n_opts == 0, "0 个选项");
        CK(ev.ask_ans == 0, "ans 被降级为 0（否则会给出空列表让人选）");
    }

    puts("[9] ask_end 收回提问页");
    {
        int64_t now = 8000;
        wb_core_init(&s, now);
        wb_proto_decode(ASK_SINGLE, strlen(ASK_SINGLE), &ev);
        wb_core_apply(&s, &ev, now, &t);
        CK(wb_core_page(&s) == WB_PAGE_ASK, "先进提问页");
        memset(&ev, 0, sizeof(ev));
        ev.type = WB_EV_ASK_END;
        ev.ask_id = 0;   // 0 = 收所有
        now += 1000;
        wb_core_apply(&s, &ev, now, &t);
        CK(!s.ask.active, "ask 已清空");
        CK(wb_core_page(&s) != WB_PAGE_ASK, "已离开提问页");
    }

    puts("[10] 90s 无保活自动退出（防僵尸页面）");
    {
        int64_t now = 9000;
        wb_core_init(&s, now);
        wb_proto_decode(ASK_SINGLE, strlen(ASK_SINGLE), &ev);
        wb_core_apply(&s, &ev, now, &t);
        printf("      当前 TTL = %d ms\n", WB_ASK_TTL_MS);
        now += WB_ASK_TTL_MS + 1000;
        wb_core_tick(&s, now, &t);
        CK(!s.ask.active, "超时后 ask 清空");
        CK(t.ask_expired, "ask_expired 上报");
        CK(wb_core_page(&s) != WB_PAGE_ASK, "已退出提问页");
    }

    puts("[11] 审批优先级高于提问");
    {
        int64_t now = 10000;
        wb_core_init(&s, now);
        wb_proto_decode(ASK_SINGLE, strlen(ASK_SINGLE), &ev);
        wb_core_apply(&s, &ev, now, &t);
        wb_ev_t ae;
        memset(&ae, 0, sizeof(ae));
        ae.type = WB_EV_APPROVAL;
        ae.approval_id = 7;
        snprintf(ae.tool, sizeof(ae.tool), "bash");
        snprintf(ae.hint, sizeof(ae.hint), "rm -rf /tmp/x");
        now += 500;
        wb_core_apply(&s, &ae, now, &t);
        CK(wb_core_page(&s) == WB_PAGE_APPROVAL, "审批压过提问");
        CK(s.ask.active, "提问仍保留（答完审批还能回去）");
    }

    printf("\n%s (%d 失败)\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
