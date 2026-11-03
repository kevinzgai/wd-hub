// main/apps/voice/nvs_settings.h —— 持久化设置(NVS 命名空间 "app")。
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_settings_init(void);

// 时区偏移小时(int8,±12;缺省 8)
esp_err_t nvs_settings_get_tz_hour(int8_t *hour);
esp_err_t nvs_settings_set_tz_hour(int8_t hour);
// 分钟精度时区(天气接口按坐标给出,含 +5:30 等);未存过返回 ESP_ERR_NVS_NOT_FOUND
esp_err_t nvs_settings_get_tz_min(int16_t *minutes);
esp_err_t nvs_settings_set_tz_min(int16_t minutes);

void nvs_settings_factory_reset(void);   // 清 "app" 命名空间

#ifdef __cplusplus
}
#endif
