/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(char *text, ...);
// same, but buffered: flushed at most every 250 ms (engine print() chatter)
int debugPrintfLazy(char *text, ...);

void cpu_boost(int on);

// the engine reads its stack-protector canary from tpidr_el0 + 0x28 (bionic).
void tls_setup_guard(void);

int ret0(void);
int retm1(void);

// own process handle for the code-memory svcs: the real one when the loader
// passed it, otherwise the CurrentProcess pseudo-handle (the kernel resolves it
// to this process for svcMap/UnmapProcessCodeMemory and
// svcSetProcessMemoryPermission)
uint32_t own_process_handle(void);

// 1 once the wrapper owns the display through EGL (text console unusable)
extern int g_gfx_owned;
// 0 = debugPrintf writes to the log file only (crash handler)
extern int g_log_stdout;

static inline void* armGetTlsRw(void) {
  void* ret;
  __asm__ ("mrs %x[data], s3_3_c13_c0_2" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#endif
