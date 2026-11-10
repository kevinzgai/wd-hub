#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void *context;
    bool (*valid)(void *context);
    int (*send)(void *context);
    bool (*retryable)(int code);
    uint32_t (*now_ms)(void *context);
    void (*wait_ms)(void *context, uint32_t delay_ms);
} ble_tx_retry_ops_t;

typedef struct {
    int code;
    uint32_t attempts;
    uint32_t elapsed_ms;
    bool cancelled;
} ble_tx_retry_result_t;

// Worker context only. Retry a rejected enqueue, never an accepted notification.
// The validity callback must bind both connection epoch and recording session.
ble_tx_retry_result_t ble_tx_retry_send(const ble_tx_retry_ops_t *ops,
                                      uint32_t timeout_ms, uint32_t interval_ms);
