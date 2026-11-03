#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) (++*(mux))
#define taskEXIT_CRITICAL(mux) (--*(mux))
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
static inline void test_log(const char *tag,const char *format,...){(void)tag;(void)format;}
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
static inline const char *esp_err_to_name(int n){(void)n;return "error";}
int xTaskCreate(void (*fn)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(unsigned);
unsigned ulTaskNotifyTake(int,unsigned);
void xTaskNotifyGive(TaskHandle_t);
int bsp_audio_init(void);
int bsp_audio_set_format(unsigned,uint8_t,uint8_t);
int bsp_audio_write(const void *,size_t);
int bsp_audio_read(void *,size_t);
