#pragma once
#include "FreeRTOS.h"
typedef struct { int unused; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf) { return buf; }
static inline int xSemaphoreTake(SemaphoreHandle_t h, unsigned ticks) { (void)h; (void)ticks; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t h) { (void)h; return 1; }
