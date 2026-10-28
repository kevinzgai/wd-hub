/* 芯片温度 + 板载指标上报 —— 实现（从 main.cpp 原样搬出） */
#include "device_report.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "driver/temperature_sensor.h"

#include "battery.h"
#include "wifi_mgr.h"

static const char *TAG = "device";

// 温度传感器只在启动时装一次，句柄常驻。
// ESP32-S3 内置的其实是**射频用的**温度传感器，精度 ±1°C 左右，
// 测的是芯片结温（die temperature），不是环境温度 ——
// 芯片自己发热，读数天然比室温高 5~15°C，满载时更高。
// 所以它适合回答"芯片有没有过热"，不能当室温计用。
static temperature_sensor_handle_t s_tsens = NULL;

/* 最近一次读数。UI 读它，不碰传感器句柄（见头文件说明）。 */
static volatile int s_chip_temp = -1;

static void tsens_init(void)
{
    // 范围给 -10~80：芯片结温正常在 30~70，超出这个范围要么是没焊好，
    // 要么是真的过热，两种情况都该被看到而不是被钳在边界上。
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "温度传感器不可用: %s（页脚将显示 --）", esp_err_to_name(err));
        s_tsens = NULL;
        return;
    }
    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "温度传感器使能失败: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        return;
    }
    ESP_LOGI(TAG, "温度传感器就绪");
}

static int tsens_read(void)
{
    if (!s_tsens) return -1;
    float c = 0;
    if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) return -1;
    return (int)(c + (c >= 0 ? 0.5f : -0.5f));   // 四舍五入到整数
}

void device_report_init(void)
{
    tsens_init();
}

int device_report_chip_temp(void)
{
    return s_chip_temp;
}

// 把板载指标 POST 给服务端。
//
// 为什么用 POST 而不是塞进 GET 的 query：
//   GET /status 是**服务端 → 设备**的通道，且被标记为冻结契约。
//   上报是反方向的，混在一起会让"设备只读不写"和"设备只写不读"
//   两种用法互相牵连。独立端点出问题时也不会影响看板刷新。
//
// 为什么失败不重试：
//   30 秒后又报一次，重试的边际价值几乎为零，反而可能拖住 fetch 任务。
//   温度不是告警数据，晚一轮没有后果。
void device_report_send(void)
{
    const char *base = wifi_mgr_svr_base();
    if (!base || !base[0]) return;

    int t = tsens_read();
    s_chip_temp = t;

    battery_state_t b;
    battery_get(&b);

    // JSON 手工拼：只有 4 个字段，上 cJSON 要多分配一次堆，
    // 而这函数在内存紧张的看板固件里每 30 秒跑一次，能省就省。
    char body[128];
    snprintf(body, sizeof(body),
             "{\"chip_temp\":%d,\"battery\":%d,\"charging\":%s,\"uptime_s\":%lld}",
             t < 0 ? 0 : t,              // 传感器坏了就先报 0，服务端另有 -1 语义
             b.percent,
             b.charging ? "true" : "false",
             (long long)(esp_timer_get_time() / 1000000));

    char url[96];
    snprintf(url, sizeof(url), "%s/device/report", base);

    esp_http_client_config_t cfg = {};
    cfg.url        = url;
    cfg.method     = HTTP_METHOD_POST;
    cfg.timeout_ms = 4000;          // 比 GET 短：上报失败无所谓，别拖时间

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return;

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(cli);
    int code = (err == ESP_OK) ? esp_http_client_get_status_code(cli) : 0;
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || code / 100 != 2) {
        // 用 WARN 级别：这条链路上行静默失败了很久没人发现，
        // 而它一轮只打一次，不会淹没日志。DEBUG 级别的代价是
        // 「服务端 chip_temp 一直 -1」时串口上一点线索都没有。
        ESP_LOGW(TAG, "上报失败 err=%s code=%d body=%s",
                 esp_err_to_name(err), code, body);
    } else {
        ESP_LOGI(TAG, "上报 chip_temp=%d bat=%d%% code=%d", t, b.percent, code);
    }
}
