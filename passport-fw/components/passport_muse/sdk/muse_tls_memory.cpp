#include "muse_tls_memory.h"
#include "mbedtls/esp_mbedtls_dynamic.h"
#include "mbedtls/platform_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>

// The IDF 5.5.3 RX reservation for an 8192-byte content limit needs 8866
// bytes including its metadata. Keep the SDK responsible for all headers,
// offsets and destruction; only supply storage during that one public call.
// A configuration increase fails closed instead of falling back to the heap.
namespace {
alignas(16) unsigned char rx_storage[9 * 1024];
std::atomic<TaskHandle_t> reserving_task{nullptr};
std::atomic<bool> rx_in_use{false};
}

extern "C" void *__real_esp_mbedtls_mem_calloc(size_t count, size_t size);
extern "C" void __real_esp_mbedtls_mem_free(void *ptr);

extern "C" void *__wrap_esp_mbedtls_mem_calloc(size_t count, size_t size) {
    const auto owner = reserving_task.load();
    if (owner && owner == xTaskGetCurrentTaskHandle()) {
        if (!count || !size || count > std::numeric_limits<size_t>::max() / size
            || count * size > sizeof(rx_storage)) return nullptr;
        bool unused = false;
        if (!rx_in_use.compare_exchange_strong(unused, true)) return nullptr;
        std::memset(rx_storage, 0, sizeof(rx_storage));
        return rx_storage;
    }
    return __real_esp_mbedtls_mem_calloc(count, size);
}

extern "C" void __wrap_esp_mbedtls_mem_free(void *ptr) {
    if (ptr == rx_storage) {
        // Keep the slot owned until secret record contents are erased.
        mbedtls_platform_zeroize(rx_storage, sizeof(rx_storage));
        rx_in_use.store(false);
        return;
    }
    __real_esp_mbedtls_mem_free(ptr);
}

esp_err_t muse_tls_reserve_rx(mbedtls_ssl_context *ssl) {
    if (!ssl || rx_in_use.load()) return ESP_ERR_INVALID_STATE;
    TaskHandle_t empty = nullptr;
    if (!reserving_task.compare_exchange_strong(empty, xTaskGetCurrentTaskHandle()))
        return ESP_ERR_INVALID_STATE;
    const esp_err_t result = esp_mbedtls_dynamic_set_rx_buf_static(ssl);
    reserving_task.store(nullptr);
    if (result == ESP_OK && !rx_in_use.load()) return ESP_ERR_INVALID_STATE;
    return result;
}
