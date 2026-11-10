#pragma once

#include <stdbool.h>

typedef struct {
    void *context;
    bool (*valid)(void *context);
    // submit=true sends ptt_up; false sends a session-scoped device_error.
    int (*notify)(void *context, bool submit);
    int (*disconnect)(void *context);
} ble_capture_end_ops_t;

typedef struct {
    int notify_rc;
    int disconnect_rc;
    bool cancelled;
    bool disconnect_requested;
} ble_capture_end_result_t;

// Worker context. Failed capture must never be submitted as an ordinary release.
// If the terminal notification cannot be queued, disconnect that same session
// so the host cannot keep an abandoned capture open. Never touch a newer link.
ble_capture_end_result_t ble_capture_end(const ble_capture_end_ops_t *ops, bool submit);
