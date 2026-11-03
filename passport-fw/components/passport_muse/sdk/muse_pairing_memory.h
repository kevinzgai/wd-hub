#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Borrowed until BLE host and dispatch workers have stopped.
uint8_t *muse_pairing_acquire_rx(size_t bytes);
// Non-overlapping decode scratch; caller must hold the pairing crypto lock.
uint8_t *muse_pairing_cipher_buffer(size_t bytes);
char *muse_pairing_decode_buffer(size_t bytes);
// 16-byte aligned stack for the BLE command worker, inside the same borrowed
// region: a fragmented C3 heap often has no contiguous 4 KB block left.
#define MUSE_PAIRING_STACK_BYTES 4096
uint8_t *muse_pairing_stack(void);
void muse_pairing_release_rx(void);

#ifdef __cplusplus
}
#endif
