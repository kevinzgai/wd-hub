#include <stdio.h>
#include <string.h>
#include "wb_proto.h"

static int fails = 0;
#define CK(cond, msg) do { if (!(cond)) { printf("  ✗ %s\n", msg); fails++; } \
                           else printf("  ✓ %s\n", msg); } while (0)

int main(void)
{
    wb_ev_t ev;
    char frame[512];

    puts("[1] 正常 usage 帧");
    {
        snprintf(frame, sizeof(frame),
            "{\"usage\":1,\"today\":3560720,\"req\":32,\"cache\":1425600,"
            "\"models\":["
            "{\"n\":\"trae/d…4.1-flash\",\"v\":1187633,\"hit\":0},"
            "{\"n\":\"deepse…4.1-flash\",\"v\":889009,\"hit\":602240},"
            "{\"n\":\"deepseek-flash\",\"v\":719828,\"hit\":358784},"
            "{\"n\":\"qodercn/dfmodel\",\"v\":526343,\"hit\":374144},"
            "{\"n\":\"其它\",\"v\":237907,\"hit\":90432}]}");
        CK(wb_proto_decode(frame, strlen(frame), &ev), "解码成功");
        CK(ev.type == WB_EV_USAGE, "类型 = USAGE");
        CK(ev.usage.valid, "valid 置位");
        CK(ev.usage.today == 3560720, "today = 3560720");
        CK(ev.usage.req == 32, "req = 32");
        CK(ev.usage.cached == 1425600, "cache = 1425600");
        CK(ev.usage.n_model == 5, "模型 5 条");
        CK(strcmp(ev.usage.model[0].name, "trae/d…4.1-flash") == 0, "名字含省略号未丢字节");
        CK(ev.usage.model[4].tokens == 237907, "第5条用量正确");
        long long sum = 0;
        for (int i = 0; i < ev.usage.n_model; i++) sum += ev.usage.model[i].tokens;
        printf("      模型合计 %lld vs today %lld\n", sum, (long long)ev.usage.today);
        CK(sum == ev.usage.today, "模型之合 == 总量");
    }

    puts("[2] 只有总量、无 models");
    {
        snprintf(frame, sizeof(frame),
            "{\"usage\":1,\"today\":12345,\"req\":3,\"cache\":0}");
        CK(wb_proto_decode(frame, strlen(frame), &ev), "解码成功");
        CK(ev.usage.n_model == 0, "模型 0 条");
        CK(ev.usage.today == 12345, "today 正确");
        CK(ev.usage.valid, "valid 仍置位（无模型不算失败）");
    }

    puts("[3] 超量模型（7 条）只取前 5");
    {
        char m[600];
        strcpy(m, "{\"usage\":1,\"today\":100,\"req\":7,\"cache\":0,\"models\":[");
        for (int i = 0; i < 7; i++) {
            char one[64];
            snprintf(one, sizeof(one), "%s{\"n\":\"m%d\",\"v\":%d,\"hit\":0}",
                     i ? "," : "", i, 100 - i);
            strcat(m, one);
        }
        strcat(m, "]}");
        CK(wb_proto_decode(m, strlen(m), &ev), "解码成功（不越界）");
        CK(ev.usage.n_model == 5, "截到 5 条");
        CK(strcmp(ev.usage.model[4].name, "m4") == 0, "第5条是 m4");
    }

    puts("[4] 空名字条目被跳过");
    {
        snprintf(frame, sizeof(frame),
            "{\"usage\":1,\"today\":100,\"req\":2,\"cache\":0,\"models\":["
            "{\"n\":\"\",\"v\":50,\"hit\":0},{\"n\":\"ok\",\"v\":50,\"hit\":0}]}");
        CK(wb_proto_decode(frame, strlen(frame), &ev), "解码成功");
        CK(ev.usage.n_model == 1, "只留 1 条（空名跳过）");
        CK(strcmp(ev.usage.model[0].name, "ok") == 0, "留下的是 ok");
    }

    puts("[5] 积分帧不能被 usage 抢走");
    {
        snprintf(frame, sizeof(frame), "{\"remain\":742.5,\"total\":1000.0}");
        CK(wb_proto_decode(frame, strlen(frame), &ev), "解码成功");
        CK(ev.type == WB_EV_CREDIT, "类型仍是 CREDIT");
        CK(ev.remain == 742.5, "remain 正确");
    }

    puts("[6] 大数值 int64 不溢出");
    {
        snprintf(frame, sizeof(frame),
            "{\"usage\":1,\"today\":3000000000,\"req\":5,\"cache\":0}");
        CK(wb_proto_decode(frame, strlen(frame), &ev), "解码成功");
        printf("      today = %lld\n", (long long)ev.usage.today);
        CK(ev.usage.today == 3000000000LL, "3e9 正确（超过 int32）");
    }

    puts("[7] 语音轮询帧仍被安静忽略");
    {
        snprintf(frame, sizeof(frame), "{\"voice\":\"poll\"}");
        CK(wb_proto_decode(frame, strlen(frame), &ev), "解码成功");
        CK(ev.type == WB_EV_NONE, "类型 = NONE");
    }

    printf("\n%s (%d 失败)\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
