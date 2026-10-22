/* prof.h -- cheap always-on counters for where load stalls go (file system
 * vs. reads vs. GL shader compilation vs. texture uploads). Times are summed
 * across threads, so they can exceed wall time when worker threads overlap.
 * MIT license; see LICENSE. */

#ifndef __PROF_H__
#define __PROF_H__

#include <stdint.h>

enum {
  PROF_OPEN,    // fopen/open/opendir/AAssetManager_open
  PROF_STAT,    // stat/lstat/fstat/access
  PROF_READ,    // fread/read/AAsset_read
  PROF_SHADER,  // glCompileShader/glLinkProgram/glProgramBinary
  PROF_TEX,     // glTexImage2D/glTexSubImage*/glCompressedTex*
  PROF_N
};

uint64_t prof_now(void);
void prof_add(int cat, uint64_t t0);

// a snapshot of the counters; prof_report() logs the delta since `prev`
typedef struct { uint64_t ns[PROF_N]; uint64_t n[PROF_N]; } ProfSnap;
void prof_snapshot(ProfSnap *s);
void prof_report(const char *what, unsigned long long wall_ms, const ProfSnap *prev, const ProfSnap *now);

#endif
