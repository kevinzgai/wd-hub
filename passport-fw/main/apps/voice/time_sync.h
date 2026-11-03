// main/time_sync.h —— wall-clock 校时(校时源仅电脑客户端,双通道 BLE/USB)。
// 设计要点(见 design.md):
//   - s_epoch(单调缓存)为唯一真源:settimeofday 顺带同步系统时间(供未来日志
//     时间戳选项),UI/格式化一律读缓存 —— 宿主测试零系统时间副作用;
//   - 时区为分钟偏移:首页天气按城市坐标自动给出(出国/跨时区自动跟随),
//     NVS 存 tz_min;老的整小时 tz_hour 仍兼容。不做 TZ env;
//   - 重启后未校时:valid=false → UI 显示 "--:--",relay 连接后恢复。
#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_sync_init(void);                // 读 NVS 时区(缺省 +8);校时状态=未校时
void time_sync_set_epoch(int64_t epoch_sec);   // 校时(UTC 秒);置 valid + 同步系统时间
bool time_sync_valid(void);                    // 是否已校时(UI "--:--" 判定)
int64_t time_sync_last_epoch(void);
int time_sync_tz_hour(void);                   // 当前时区偏移小时(默认 8)
esp_err_t time_sync_set_tz(int hour);          // 设置时区偏移(±12 校验,NVS 持久化)
int time_sync_tz_offset_sec(void);             // 当前时区偏移秒(本地 = UTC + 此值)
// 天气接口给出的偏移(分钟,-720..840);与当前相同则不写 NVS。
esp_err_t time_sync_set_tz_offset_min(int minutes);
int time_sync_format_local(char *buf, size_t cap); // "HH:MM"(校时)或 "--:--"(未校时)

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_H */
