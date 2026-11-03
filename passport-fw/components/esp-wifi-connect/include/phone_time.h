#pragma once
#include <stdint.h>
#include <math.h>
#include <cJSON.h>

// Optional main-owned clock bridge; the Wi-Fi component has no main dependency.
extern "C" void passport_phone_time(int64_t epoch, int tz_minutes) __attribute__((weak));
inline bool wifi_phone_time_parse(const cJSON *json, int64_t *epoch, int *minutes) {
    const auto *t = cJSON_GetObjectItemCaseSensitive(json, "epoch");
    const auto *z = cJSON_GetObjectItemCaseSensitive(json, "tz_minutes");
    if (!cJSON_IsNumber(t) || !cJSON_IsNumber(z)) return false;
    const double sec = t->valuedouble, tz = z->valuedouble;
    if (!isfinite(sec) || sec < 1700000000 || sec > 4102444800 || floor(sec) != sec ||
        !isfinite(tz) || tz < -720 || tz > 840 || floor(tz) != tz || (int)tz % 15) return false;
    *epoch = (int64_t)sec; *minutes = (int)tz;
    return true;
}
inline void wifi_phone_time_apply(const cJSON *json) {
    int64_t epoch; int minutes;
    if (passport_phone_time && wifi_phone_time_parse(json, &epoch, &minutes))
        passport_phone_time(epoch, minutes);
}
