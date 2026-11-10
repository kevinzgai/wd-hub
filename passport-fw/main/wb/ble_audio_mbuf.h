#pragma once

#include <stddef.h>
#include "os/os_mbuf.h"

// Allocate audio payloads from the larger MSYS pool. NimBLE still allocates ATT
// headers, control and RX from the small pool; this is not a control reservation.
// Caller transfers ownership to NimBLE.
struct os_mbuf *ble_audio_mbuf_from_flat(const void *data, size_t length);
