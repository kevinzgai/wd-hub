#pragma once
static inline void bsp_memory_phase(const char *) {}
inline unsigned mock_alloc_failures = 0;
static inline unsigned bsp_memory_failures(void) { return mock_alloc_failures; }
