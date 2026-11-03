#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Phase labels must have static lifetime. Sampling is not an allocation trace.
void bsp_memory_init(void);
void bsp_memory_phase(const char *phase);
void bsp_memory_sample(void);
void bsp_memory_dump(void);
// Replay the bounded ring without consuming normal phase diagnostics.
void bsp_memory_dump_recent_failures(void);
// Allocation failures since boot; lets callers tell whether a phase hit one.
unsigned bsp_memory_failures(void);
// Diagnostic build only: print every internal heap block with its owner task.
void bsp_memory_map(void);
#ifdef __cplusplus
}
#endif
