/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"

void fatal_error(const char *fmt, ...) {
  // log first: once the wrapper owns the EGL window the console below may not
  // be able to show anything, and the process would just vanish
  {
    char msg[512];
    va_list l;
    va_start(l, fmt);
    vsnprintf(msg, sizeof(msg), fmt, l);
    va_end(l);
    debugPrintf("!!!!! FATAL: %s\n", msg);
  }

  if (g_gfx_owned) {
    // EGL owns the window: show the message through the system error applet,
    // which draws on its own layer regardless of our graphics state
    char msg[512];
    va_list l;
    va_start(l, fmt);
    vsnprintf(msg, sizeof(msg), fmt, l);
    va_end(l);
    ErrorApplicationConfig c;
    if (R_SUCCEEDED(errorApplicationCreate(&c, "Super Mario Bros. Remastered failed to start.", msg)))
      errorApplicationShow(&c);
    exit(1);
  }

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  va_list list;
  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
