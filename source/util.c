/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log = NULL; // persistent log handle (fast; fflush per line)

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

// sdmc is mounted by the time userAppInit runs, so open the log once here
// instead of reopening it per line (the engine logs thousands of lines).
void userAppInit(void) {
  initNxLink();
  s_log = fopen(LOG_PATH, "w");
  if (!s_log) s_log = fopen(LOG_NAME, "w"); // fall back to the launch CWD
  if (s_log) {
    fputs("== smbr log open ==\n", s_log);
    fflush(s_log);
  }
}

void userAppExit(void) {
  if (s_log) { fclose(s_log); s_log = NULL; }
  deinitNxLink();
}

#endif

#if DEBUG_LOG
// engine chatter (print() output) is buffered and flushed at most every 250 ms;
// wrapper lines, engine errors and the crash dump still flush immediately
static int s_lazy_line = 0;
static u64 s_last_flush = 0;
#endif

int debugPrintfLazy(char *text, ...) {
#if DEBUG_LOG
  if (!s_log) return 0;
  s_lazy_line = 1;
  va_list list;
  va_start(list, text);
  char buf[0x900];
  vsnprintf(buf, sizeof(buf), text, list);
  va_end(list);
  debugPrintf("%s", buf);
  s_lazy_line = 0;
#else
  (void)text;
#endif
  return 0;
}

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;

  if (s_log) {
    // timestamp each line start (ms since the log opened)
    static u64 t0;
    static int at_line_start = 1;
    if (!t0) t0 = armGetSystemTick();
    if (at_line_start)
      fprintf(s_log, "[%7llu] ", (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
    const size_t tl = strlen(text);
    at_line_start = tl > 0 && text[tl - 1] == '\n';
    va_start(list, text);
    vfprintf(s_log, text, list);
    va_end(list);
    // flush each line so a crash still leaves a complete log, except lazy
    // (engine print) lines, which flush at most every 250 ms
    const u64 now = armGetSystemTick();
    if (!s_lazy_line || armTicksToNs(now - s_last_flush) > 250000000ull) {
      fflush(s_log);
      s_last_flush = now;
    }
  }

  if (g_log_stdout) {
    va_start(list, text);
    vprintf(text, list);
    va_end(list);
  }
#endif
  return 0;
}

// Shared TLS block for the engine stack-protector guard at tpidr_el0 + 0x28.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  armSetTlsRw(s_tls_block);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int g_gfx_owned = 0;
int g_log_stdout = 1;

int ret0(void) { return 0; }

int retm1(void) { return -1; }

uint32_t own_process_handle(void) {
  const Handle h = envGetOwnProcessHandle();
  return h != INVALID_HANDLE ? h : CUR_PROCESS_HANDLE;
}
