#include "ble_capture_end.h"

ble_capture_end_result_t ble_capture_end(const ble_capture_end_ops_t *ops, bool submit)
{
    ble_capture_end_result_t result = {0};
    if (!ops->valid(ops->context)) {
        result.cancelled = true;
        return result;
    }
    result.notify_rc = ops->notify(ops->context, submit);
    if (!ops->valid(ops->context)) {
        result.cancelled = true;
        return result;
    }
    if (result.notify_rc != 0) {
        result.disconnect_requested = true;
        result.disconnect_rc = ops->disconnect(ops->context);
    }
    return result;
}
