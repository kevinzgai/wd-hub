#pragma once
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t s, unsigned ticks);
int xSemaphoreGive(SemaphoreHandle_t s);
