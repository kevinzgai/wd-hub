#pragma once
#include "registry_platform.h"
enum esp_reset_reason_t { ESP_RST_POWERON = 1, ESP_RST_SW = 3, ESP_RST_PANIC = 4 };
inline esp_reset_reason_t mock_reset_reason = ESP_RST_POWERON;
inline esp_reset_reason_t esp_reset_reason() { return mock_reset_reason; }
struct MockRestart {};
inline int mock_restarts = 0;
[[noreturn]] inline void esp_restart() { ++mock_restarts; throw MockRestart{}; }
