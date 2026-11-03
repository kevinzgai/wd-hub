// main/time_sync.c —— wall-clock 校时(校时源仅电脑客户端)。
// 三通道下发统一入口:BLE 协议行 time.set、USB SYS 命令 time set、REPL 手动。
// 状态:重启后未校时(valid=false);set_epoch 置位后本会话有效。
// 时区:分钟偏移 s_tz_min。NVS 键 tz_min(天气接口自动设置)优先,否则用老的
// tz_hour(int8,默认 +8,±12 校验)。不引入 TZ env。
#include "time_sync.h"
#include "nvs_settings.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>  // settimeofday
#include <stdio.h>

static const char *TAG = "time_sync";

#define TZ_DEFAULT_HOUR 8
#define TZ_MAX_HOUR     12
#define TZ_MIN_MINUTES  (-720)
#define TZ_MAX_MINUTES  840   /* UTC+14(基里巴斯) */

// s_epoch 为校时基线(单写点:set_epoch);settimeofday 同步系统时钟并持续
// 走时 —— 设备端显示读 time(NULL)(见 format_local),否则永远冻结在校时
// 时刻(真机实测 13:46 不再走动)。宿主测试无系统时钟,按 s_epoch 直接格式化。
static int64_t s_epoch = 0;
static bool s_valid = false;
static int s_tz_min = TZ_DEFAULT_HOUR * 60;

esp_err_t time_sync_init(void)
{
    int16_t minutes = 0;
    if (nvs_settings_get_tz_min(&minutes) == ESP_OK) {
        s_tz_min = minutes;
    } else {
        int8_t tz = 0;
        nvs_settings_get_tz_hour(&tz);   // 内部已兜底:打不开/损坏/缺省 → 8
        s_tz_min = ((tz >= -TZ_MAX_HOUR && tz <= TZ_MAX_HOUR) ? tz : TZ_DEFAULT_HOUR) * 60;
    }
    s_valid = false;
    return ESP_OK;
}

void time_sync_set_epoch(int64_t epoch_sec)
{
    s_epoch = epoch_sec;
    s_valid = true;
#ifdef ESP_PLATFORM
    struct timeval tv = { .tv_sec = (time_t)epoch_sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);   // 失败忽略:UI 走 s_epoch,系统时间仅日志时间戳备用
#endif
}

bool time_sync_valid(void) { return s_valid; }
int64_t time_sync_last_epoch(void) { return s_epoch; }

void passport_phone_time(int64_t epoch, int minutes) {
    if (epoch < 1700000000LL || epoch > 4102444800LL ||
        minutes < -720 || minutes > 840 || minutes % 15) return;
    if (time(NULL) < 1700000000LL) time_sync_set_epoch(epoch);
    time_sync_set_tz_offset_min(minutes);
}

int time_sync_tz_hour(void) { return s_tz_min / 60; }

int time_sync_tz_offset_sec(void) { return s_tz_min * 60; }

esp_err_t time_sync_set_tz_offset_min(int minutes)
{
    if (minutes < TZ_MIN_MINUTES || minutes > TZ_MAX_MINUTES) return ESP_ERR_INVALID_ARG;
    if (minutes == s_tz_min) return ESP_OK;   // 每次天气刷新都会调,不重复写 flash
    ESP_LOGI(TAG, "时区自动更新 %+d -> %+d 分钟", s_tz_min, minutes);
    s_tz_min = minutes;
    return nvs_settings_set_tz_min((int16_t)minutes);
}

esp_err_t time_sync_set_tz(int hour)
{
    if (hour < -TZ_MAX_HOUR || hour > TZ_MAX_HOUR) {
        ESP_LOGW(TAG, "时区偏移越界 %d(±%d)", hour, TZ_MAX_HOUR);
        return ESP_ERR_INVALID_ARG;
    }
    s_tz_min = hour * 60;
    nvs_settings_set_tz_min((int16_t)s_tz_min);   // 手动设置覆盖自动值
    return nvs_settings_set_tz_hour((int8_t)hour);
}

// 本地时间 = 当前系统时钟 + tz*3600,按 UTC 读(gmtime_r 手动偏移,不依赖
// TZ env)。设备端系统时钟 set_epoch 时已 settimeofday,持续走时;宿主测试
// 无系统时钟,按 s_epoch 基线直接格式化(确定性基准)。返回写入长度
// (未校时固定 "--:--" 5 字符)。
int time_sync_format_local(char *buf, size_t cap)
{
    if (!s_valid) return snprintf(buf, cap, "--:--");
    time_t t;
#ifdef ESP_PLATFORM
    time_t now = time(NULL);
    if (now < s_epoch) now = (time_t)s_epoch;   // settimeofday 未生效兜底:冻结在基线
    t = now + (int64_t)s_tz_min * 60;
#else
    t = (time_t)(s_epoch + (int64_t)s_tz_min * 60);
#endif
    struct tm tm;
    gmtime_r(&t, &tm);
    return snprintf(buf, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}
