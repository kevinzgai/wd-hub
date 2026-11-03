#include "bsp_memory.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    size_t bytes;
    uint32_t caps;
    const char *function;
    const char *phase;
    unsigned sequence;
} failure_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *s_phase = "boot";
static uint32_t s_generation;
static size_t s_min_free = SIZE_MAX, s_min_largest = SIZE_MAX, s_min_dma = SIZE_MAX;
static failure_t s_failures[4];
static unsigned s_failed, s_reported;
// Largest single request and number of >=4 KiB requests in the current phase.
// Only the diagnostic build (CONFIG_HEAP_USE_HOOKS) feeds these.
static volatile size_t s_peak_request;
static volatile unsigned s_big_requests;

#if CONFIG_HEAP_USE_HOOKS
// Runs on every successful allocation, possibly with the flash cache disabled:
// IRAM (placed by the header declaration), no locks. A lost update between
// concurrent allocators only under-reports one sample, fine for a peak.
void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps) {
    (void)ptr; (void)caps;
    if (size > s_peak_request) s_peak_request = size;
    if (size >= 4096) s_big_requests = s_big_requests + 1;
}
#endif

// No allocator, formatting, logging, heap walk or task lookup in this hook.
static void allocation_failed(size_t bytes, uint32_t caps, const char *function) {
    portENTER_CRITICAL_SAFE(&s_lock);
    unsigned sequence = ++s_failed;
    s_failures[(sequence - 1) % 4] = (failure_t){bytes, caps, function, s_phase, sequence};
    portEXIT_CRITICAL_SAFE(&s_lock);
}
void bsp_memory_init(void) {
    if (heap_caps_register_failed_alloc_callback(allocation_failed) != ESP_OK)
        ESP_LOGW("memory", "Allocation failure hook unavailable");
#if CONFIG_HEAP_TASK_TRACKING || CONFIG_HEAP_USE_HOOKS
    ESP_LOGW("memory", "Heap diagnostic build: not for release");
#endif
}
unsigned bsp_memory_failures(void) {
    portENTER_CRITICAL_SAFE(&s_lock);
    unsigned failures = s_failed;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return failures;
}
void bsp_memory_sample(void) {
    portENTER_CRITICAL_SAFE(&s_lock);
    uint32_t generation = s_generation;
    portEXIT_CRITICAL_SAFE(&s_lock);
    size_t total = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    portENTER_CRITICAL_SAFE(&s_lock);
    if (generation == s_generation) {
        if (total < s_min_free) s_min_free = total;
        if (largest < s_min_largest) s_min_largest = largest;
        if (dma < s_min_dma) s_min_dma = dma;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}
void bsp_memory_dump(void) {
    bsp_memory_sample();
    failure_t recent[4];
    unsigned count = 0;
    portENTER_CRITICAL_SAFE(&s_lock);
    const char *phase = s_phase;
    size_t total = s_min_free, largest = s_min_largest, dma = s_min_dma;
    unsigned failures = s_failed;
    unsigned first = failures - s_reported > 4 ? failures - 4 : s_reported;
    for (unsigned i = first; i < failures; ++i) recent[count++] = s_failures[i % 4];
    s_reported = failures;
    portEXIT_CRITICAL_SAFE(&s_lock);
    ESP_LOGI("memory", "phase=%s sampled_min=%u largest_min=%u dma_min=%u failures=%u",
             phase, (unsigned)total, (unsigned)largest, (unsigned)dma, failures);
#if CONFIG_HEAP_USE_HOOKS
    ESP_LOGI("memory", "phase=%s peak_request=%u big_requests=%u",
             phase, (unsigned)s_peak_request, s_big_requests);
#endif
    for (unsigned i = 0; i < count; ++i) {
        const failure_t *f = &recent[i];
        ESP_LOGW("memory", "alloc_failed seq=%u phase=%s bytes=%u caps=0x%lx function=%s",
                 f->sequence, f->phase, (unsigned)f->bytes, (unsigned long)f->caps,
                 f->function ? f->function : "unknown");
    }
}
void bsp_memory_dump_recent_failures(void) {
    failure_t recent[4];
    unsigned count = 0;
    portENTER_CRITICAL_SAFE(&s_lock);
    unsigned failures = s_failed;
    unsigned first = failures > 4 ? failures - 4 : 0;
    for (unsigned i = first; i < failures; ++i) recent[count++] = s_failures[i % 4];
    portEXIT_CRITICAL_SAFE(&s_lock);
    for (unsigned i = 0; i < count; ++i) {
        const failure_t *f = &recent[i];
        ESP_LOGW("memory", "recent_alloc_failed seq=%u phase=%s bytes=%u caps=0x%lx function=%s",
                 f->sequence, f->phase, (unsigned)f->bytes, (unsigned long)f->caps,
                 f->function ? f->function : "unknown");
    }
}
void bsp_memory_phase(const char *phase) {
    bsp_memory_dump();
    portENTER_CRITICAL_SAFE(&s_lock);
    s_phase = phase;
    ++s_generation;
    s_min_free = s_min_largest = s_min_dma = SIZE_MAX;
    s_peak_request = 0;
    s_big_requests = 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    bsp_memory_sample();
    ESP_LOGI("memory", "enter=%s", phase);
}

#if CONFIG_HEAP_TASK_TRACKING
// Heap map for finding blocks that outlive an app. The walk runs under the heap
// lock, so the callback only copies a bounded window; printing happens between
// passes. Each pass re-walks from the start and keeps the next window, so the
// map is a near-snapshot taken while the device is otherwise idle.
#define MAP_WINDOW 48
typedef struct {
    uintptr_t addr;
    uint32_t size;
    TaskHandle_t owner;
    bool used;
} map_block_t;
typedef struct {
    unsigned index, skip, count;
    map_block_t blocks[MAP_WINDOW];
} map_walk_t;
static map_walk_t s_map;

static bool map_block(walker_heap_into_t heap, walker_block_info_t block, void *user) {
    (void)heap;
    map_walk_t *w = user;
    const unsigned i = w->index++;
    if (i < w->skip || w->count >= MAP_WINDOW) return true;
    // With task tracking the first word of a used TLSF block is its owner.
    w->blocks[w->count++] = (map_block_t){(uintptr_t)block.ptr, (uint32_t)block.size,
                                          block.used ? *(TaskHandle_t *)block.ptr : NULL,
                                          block.used};
    return true;
}

void bsp_memory_map(void) {
    printf("HM begin phase=%s free=%u largest=%u\n", s_phase,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    for (unsigned skip = 0;; skip += MAP_WINDOW) {
        s_map.index = 0;
        s_map.skip = skip;
        s_map.count = 0;
        heap_caps_walk(MALLOC_CAP_INTERNAL, map_block, &s_map);
        for (unsigned i = 0; i < s_map.count; ++i) {
            const map_block_t *b = &s_map.blocks[i];
            printf("HM B %08x %u %c %08x\n", (unsigned)b->addr, (unsigned)b->size,
                   b->used ? 'U' : 'F', (unsigned)(uintptr_t)b->owner);
        }
        if (s_map.count < MAP_WINDOW) break;
    }
    // Owners that are no longer in this table belong to deleted tasks.
    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks = malloc(cap * sizeof(*tasks));
    if (tasks) {
        const UBaseType_t n = uxTaskGetSystemState(tasks, cap, NULL);
        for (UBaseType_t i = 0; i < n; ++i)
            printf("HM T %08x %s\n", (unsigned)(uintptr_t)tasks[i].xHandle, tasks[i].pcTaskName);
        free(tasks);
    }
    printf("HM end\n");
}
#else
void bsp_memory_map(void) {
    ESP_LOGW("memory", "Heap map needs CONFIG_HEAP_TASK_TRACKING (diagnostic build)");
}
#endif
