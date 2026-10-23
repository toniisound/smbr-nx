/* sampler.c -- statistical CPU profiler for long engine steps.
 *
 * While the game thread is inside a GodotLib.step() that has already run for
 * more than SAMPLE_AFTER_MS, a sampler thread (on another core) pauses it
 * every ~2 ms, reads PC plus the frame-pointer call chain, and resumes it.
 * When the step ends, the hottest addresses are logged as module offsets
 * (godot+0x...), both "self" (where the CPU was) and "inclusive" (any frame
 * of the call chain), so they can be mapped to engine functions offline.
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <switch.h>

#include "sampler.h"
#include "so_util.h"
#include "util.h"

#define SAMPLE_AFTER_MS   100
#define SAMPLE_PERIOD_NS  (2 * 1000 * 1000)
#define MAX_SAMPLES       6000
#define MAX_DEPTH         12
#define TOP_N             25

extern so_module game_mod;
extern char __start__[]; // libnx linker script: offset 0 of this NRO's image

static Thread s_thread;
static Thread *s_target;
static volatile int s_running;
static volatile int s_in_step;
static volatile u64 s_step_t0;
static Mutex s_lock;

static u64 s_samples[MAX_SAMPLES][MAX_DEPTH + 1]; // [0] = pc, then return addresses
static int s_nsamples;

static void take_sample(void) {
  if (s_nsamples >= MAX_SAMPLES) return;
  if (R_FAILED(threadPause(s_target))) return;
  ThreadContext ctx;
  if (R_SUCCEEDED(svcGetThreadContext3(&ctx, s_target->handle))) {
    u64 *row = s_samples[s_nsamples];
    memset(row, 0, sizeof(s_samples[0]));
    row[0] = ctx.pc.x;
    u64 fp = ctx.fp;
    const u64 sp = ctx.sp;
    for (int d = 1; d <= MAX_DEPTH; d++) {
      if (fp < sp || fp > sp + (16ull << 20) || (fp & 7)) break;
      const u64 next = *(const u64 *)fp;
      const u64 ret = *(const u64 *)(fp + 8);
      if (!ret) break;
      row[d] = ret;
      if (next <= fp) break;
      fp = next;
    }
    s_nsamples++;
  }
  threadResume(s_target);
}

static void sampler_fn(void *arg) {
  (void)arg;
  while (s_running) {
    if (s_in_step) {
      const u64 ms = armTicksToNs(armGetSystemTick() - s_step_t0) / 1000000ull;
      if (ms >= SAMPLE_AFTER_MS) {
        mutexLock(&s_lock);
        if (s_in_step) take_sample();
        mutexUnlock(&s_lock);
      }
    }
    svcSleepThread(SAMPLE_PERIOD_NS);
  }
}

void sampler_start(Thread *game_thread) {
  s_target = game_thread;
  s_running = 1;
  // own core (2) so sampling doesn't steal time from the sampled thread
  if (R_SUCCEEDED(threadCreate(&s_thread, sampler_fn, NULL, NULL, 64 * 1024, 0x2B, 2)))
    threadStart(&s_thread);
  else
    s_running = 0;
}

void sampler_step_begin(void) {
  mutexLock(&s_lock);
  s_nsamples = 0;
  s_step_t0 = armGetSystemTick();
  s_in_step = 1;
  mutexUnlock(&s_lock);
}

// --- aggregation -------------------------------------------------------------

typedef struct { u64 addr; u32 self, incl; } Bucket;
#define NBUCKETS 8192
static Bucket s_buckets[NBUCKETS];

static Bucket *bucket(u64 addr) {
  u32 h = (u32)((addr >> 2) * 2654435761u) & (NBUCKETS - 1);
  for (int i = 0; i < NBUCKETS; i++) {
    Bucket *b = &s_buckets[(h + i) & (NBUCKETS - 1)];
    if (b->addr == addr) return b;
    if (!b->addr) { b->addr = addr; return b; }
  }
  return NULL;
}

static void log_top(const char *kind, int use_self, int total) {
  static u8 used[NBUCKETS];
  memset(used, 0, sizeof(used));
  const u64 base = (u64)game_mod.load_virtbase;
  for (int n = 0; n < TOP_N; n++) {
    int best = -1;
    u32 bestv = 0;
    for (int i = 0; i < NBUCKETS; i++) {
      if (!s_buckets[i].addr || used[i]) continue;
      const u32 v = use_self ? s_buckets[i].self : s_buckets[i].incl;
      if (v > bestv) { bestv = v; best = i; }
    }
    if (best < 0 || !bestv) break;
    used[best] = 1;
    const u64 a = s_buckets[best].addr;
    const u64 nro = (u64)__start__;
    if (a >= base && a < base + game_mod.load_size)
      debugPrintf("[sample] %s %3u%%  godot+0x%llx\n", kind, bestv * 100u / (u32)total, (unsigned long long)(a - base));
    else if (a >= nro && a < nro + (64ull << 20))
      debugPrintf("[sample] %s %3u%%  smbr+0x%llx\n", kind, bestv * 100u / (u32)total, (unsigned long long)(a - nro));
    else
      debugPrintf("[sample] %s %3u%%  0x%llx\n", kind, bestv * 100u / (u32)total, (unsigned long long)a);
  }
}

void sampler_step_end(int frame, unsigned long long step_ms) {
  mutexLock(&s_lock);
  s_in_step = 0;
  const int n = s_nsamples;
  mutexUnlock(&s_lock);
  if (n < 20) return; // short stall: not worth a report

  memset(s_buckets, 0, sizeof(s_buckets));
  for (int s = 0; s < n; s++) {
    Bucket *b = bucket(s_samples[s][0]);
    if (b) { b->self++; b->incl++; }
    // inclusive: each distinct return address once per sample
    for (int d = 1; d <= MAX_DEPTH && s_samples[s][d]; d++) {
      int dup = (s_samples[s][d] == s_samples[s][0]);
      for (int e = 1; e < d && !dup; e++) dup = (s_samples[s][e] == s_samples[s][d]);
      if (dup) continue;
      Bucket *c = bucket(s_samples[s][d]);
      if (c) c->incl++;
    }
  }
  debugPrintf("[sample] frame %d: %llu ms, %d samples\n", frame, step_ms, n);
  log_top("self", 1, n);
  log_top("incl", 0, n);
}
