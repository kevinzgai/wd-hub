#include "ble_tx_retry.h"

ble_tx_retry_result_t ble_tx_retry_send(const ble_tx_retry_ops_t *ops,
                                      uint32_t timeout_ms, uint32_t interval_ms)
{
    ble_tx_retry_result_t result = {0};
    const uint32_t start = ops->now_ms(ops->context);
    if (interval_ms == 0) interval_ms = 1;
    for (;;) {
        result.elapsed_ms = ops->now_ms(ops->context) - start;
        if (!ops->valid(ops->context)) {
            result.cancelled = true;
            return result;
        }
        if (result.attempts && result.elapsed_ms >= timeout_ms) return result;
        result.code = ops->send(ops->context);
        ++result.attempts;
        result.elapsed_ms = ops->now_ms(ops->context) - start;
        if (result.code == 0 || !ops->retryable(result.code) ||
            result.elapsed_ms >= timeout_ms) return result;
        uint32_t remaining = timeout_ms - result.elapsed_ms;
        ops->wait_ms(ops->context, interval_ms < remaining ? interval_ms : remaining);
    }
}
