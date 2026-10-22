/* prof.c -- see prof.h. MIT license; see LICENSE. */

#include <switch.h>

#include "prof.h"
#include "util.h"

static uint64_t s_ns[PROF_N];
static uint64_t s_n[PROF_N];

uint64_t prof_now(void) { return armGetSystemTick(); }

void prof_add(int cat, uint64_t t0) {
  const uint64_t ns = armTicksToNs(armGetSystemTick() - t0);
  __atomic_fetch_add(&s_ns[cat], ns, __ATOMIC_RELAXED);
  __atomic_fetch_add(&s_n[cat], 1, __ATOMIC_RELAXED);
}

void prof_snapshot(ProfSnap *s) {
  for (int i = 0; i < PROF_N; i++) {
    s->ns[i] = __atomic_load_n(&s_ns[i], __ATOMIC_RELAXED);
    s->n[i] = __atomic_load_n(&s_n[i], __ATOMIC_RELAXED);
  }
}

void prof_report(const char *what, unsigned long long wall_ms, const ProfSnap *a, const ProfSnap *b) {
#define D_MS(i) ((unsigned long long)((b->ns[i] - a->ns[i]) / 1000000ull))
#define D_N(i)  ((unsigned long long)(b->n[i] - a->n[i]))
  debugPrintf("[prof] %s %llu ms | open %llu ms (%llu) | stat %llu ms (%llu) | read %llu ms (%llu) | "
              "shader %llu ms (%llu) | tex %llu ms (%llu)\n",
              what, wall_ms,
              D_MS(PROF_OPEN), D_N(PROF_OPEN), D_MS(PROF_STAT), D_N(PROF_STAT),
              D_MS(PROF_READ), D_N(PROF_READ), D_MS(PROF_SHADER), D_N(PROF_SHADER),
              D_MS(PROF_TEX), D_N(PROF_TEX));
#undef D_MS
#undef D_N
}
