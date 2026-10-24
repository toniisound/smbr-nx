/* fastalloc.c -- thread-caching allocator in front of newlib's malloc for the
 * engine's malloc/free/calloc/realloc/posix_memalign imports.
 *
 * Why: newlib's malloc serializes every call on one global recursive lock.
 * Godot allocates constantly (Strings, Variants, nodes) from the main thread
 * and its loader worker threads at once, and the sampling profiler showed
 * ~4 s of every ~27 s of scene loading inside newlib malloc/free and the libnx
 * mutex calls of that lock (contention -> kernel arbitration).
 *
 * How: requests up to FA_MAX_SMALL bytes are served from per-thread free lists
 * (one per 16-byte size class) with no locking at all. Threads refill/drain
 * those lists in batches against a per-class global pool (one mutex per class),
 * which carves fresh blocks out of 64 KB slabs obtained from newlib. Blocks are
 * never returned to newlib; they are recycled. Larger requests (and alignments
 * above 16) go to newlib behind the same header.
 *
 * Every block handed out carries a 16-byte header {info, check} with
 * check = ptr ^ FA_KEY ^ info. free()/realloc() use it to tell our blocks from
 * pointers newlib allocated directly (strdup, vasprintf, realpath... results the
 * engine also frees): a newlib chunk header has the chunk size (a small number)
 * where ours keeps check, so it can't match. Foreign pointers go back to newlib.
 *
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>

#ifdef FASTALLOC_HOST_TEST
#include <pthread.h>
typedef pthread_mutex_t Mutex;
static void mutexLock(Mutex *m) { pthread_mutex_lock(m); }
static void mutexUnlock(Mutex *m) { pthread_mutex_unlock(m); }
#else
#include <switch.h>
#endif

#include "fastalloc.h"

#define FA_KEY        0x9E3779B97F4A7C15ull
#define FA_GRAIN      16
#define FA_MAX_SMALL  2048
#define FA_CLASSES    (FA_MAX_SMALL / FA_GRAIN)   // class c serves (c+1)*16 bytes
#define FA_HDR        16
#define FA_SLAB       (64 * 1024)
#define FA_BATCH      32    // blocks moved per refill/drain
#define FA_TC_MAX     128   // per-thread blocks kept per class before draining

#define FA_BIG        (1ull << 63)

typedef struct { uint64_t info, check; } FaHdr;
typedef struct FaNode { struct FaNode *next; } FaNode;

typedef struct {
  Mutex lock;
  FaNode *free;      // global pool of recycled blocks
  uint32_t nfree;
  uint8_t *slab;     // carving cursor in the current slab
  size_t slab_left;
} FaClass;

static FaClass s_cls[FA_CLASSES];

typedef struct {
  FaNode *head[FA_CLASSES];
  uint32_t count[FA_CLASSES];
} FaCache;

static __thread FaCache t_cache;

static inline FaHdr *hdr_of(void *p) { return (FaHdr *)((uint8_t *)p - FA_HDR); }

static inline void hdr_set(void *p, uint64_t info) {
  FaHdr *h = hdr_of(p);
  h->info = info;
  h->check = (uint64_t)(uintptr_t)p ^ FA_KEY ^ info;
}

static inline int is_ours(void *p) {
  const FaHdr *h = hdr_of(p);
  return h->check == ((uint64_t)(uintptr_t)p ^ FA_KEY ^ h->info);
}

static inline size_t class_size(int c) { return (size_t)(c + 1) * FA_GRAIN; }

// move up to FA_BATCH blocks of class c into the calling thread's cache
static int refill(int c) {
  FaClass *k = &s_cls[c];
  FaCache *tc = &t_cache;
  const size_t slot = FA_HDR + class_size(c);
  int got = 0;
  mutexLock(&k->lock);
  while (got < FA_BATCH && k->free) {
    FaNode *n = k->free;
    k->free = n->next;
    k->nfree--;
    n->next = tc->head[c];
    tc->head[c] = n;
    got++;
  }
  while (got < FA_BATCH) {
    if (k->slab_left < slot) {
      uint8_t *s = malloc(FA_SLAB); // newlib: 16-byte aligned
      if (!s) break;
      k->slab = s;
      k->slab_left = FA_SLAB;
    }
    void *p = k->slab + FA_HDR;
    k->slab += slot;
    k->slab_left -= slot;
    hdr_set(p, (uint64_t)c);
    FaNode *n = p;
    n->next = tc->head[c];
    tc->head[c] = n;
    got++;
  }
  mutexUnlock(&k->lock);
  tc->count[c] += (uint32_t)got;
  return got;
}

// hand FA_BATCH blocks of class c from the thread cache back to the pool
static void drain(int c) {
  FaClass *k = &s_cls[c];
  FaCache *tc = &t_cache;
  FaNode *first = tc->head[c], *last = first;
  int n = 1;
  while (n < FA_BATCH && last->next) { last = last->next; n++; }
  tc->head[c] = last->next;
  tc->count[c] -= (uint32_t)n;
  mutexLock(&k->lock);
  last->next = k->free;
  k->free = first;
  k->nfree += (uint32_t)n;
  mutexUnlock(&k->lock);
}

static void *big_alloc(size_t size, size_t align) {
  if (align < FA_GRAIN) align = FA_GRAIN;
  const size_t extra = FA_HDR + (align > FA_GRAIN ? align : 0);
  if (size > SIZE_MAX - extra) return NULL;
  uint8_t *raw = malloc(size + extra);
  if (!raw) return NULL;
  uintptr_t p = ((uintptr_t)raw + FA_HDR + align - 1) & ~(uintptr_t)(align - 1);
  hdr_set((void *)p, FA_BIG | (uint64_t)(p - (uintptr_t)raw));
  return (void *)p;
}

void *fa_malloc(size_t size) {
  if (size <= FA_MAX_SMALL) {
    const int c = size ? (int)((size - 1) / FA_GRAIN) : 0;
    FaCache *tc = &t_cache;
    if (!tc->head[c] && !refill(c)) return NULL;
    FaNode *n = tc->head[c];
    tc->head[c] = n->next;
    tc->count[c]--;
    return n;
  }
  return big_alloc(size, FA_GRAIN);
}

void fa_free(void *p) {
  if (!p) return;
  if (!is_ours(p)) { free(p); return; } // allocated by newlib directly
  const uint64_t info = hdr_of(p)->info;
  if (info & FA_BIG) {
    uint8_t *raw = (uint8_t *)p - (info & ~FA_BIG);
    hdr_of(p)->check = 0; // a stale pointer must not look like ours again
    free(raw);
    return;
  }
  const int c = (int)info;
  FaCache *tc = &t_cache;
  FaNode *n = p;
  n->next = tc->head[c];
  tc->head[c] = n;
  if (++tc->count[c] > FA_TC_MAX) drain(c);
}

void *fa_calloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) return NULL;
  void *p = fa_malloc(n * size);
  if (p) memset(p, 0, n * size);
  return p;
}

static size_t usable(void *p) {
  const uint64_t info = hdr_of(p)->info;
  if (info & FA_BIG) {
    uint8_t *raw = (uint8_t *)p - (info & ~FA_BIG);
    return malloc_usable_size(raw) - (size_t)(info & ~FA_BIG);
  }
  return class_size((int)info);
}

void *fa_realloc(void *p, size_t size) {
  if (!p) return fa_malloc(size);
  if (!is_ours(p)) return realloc(p, size); // newlib's block stays newlib's
  if (size == 0) { fa_free(p); return fa_malloc(0); }
  const uint64_t info = hdr_of(p)->info;
  const size_t old = usable(p);
  if (!(info & FA_BIG)) {
    // stay in place if the new size still maps to this class
    if (size <= old && (size > old / 2 || old == FA_GRAIN)) return p;
  } else if ((info & ~FA_BIG) == FA_HDR && size > FA_MAX_SMALL) {
    // plain big block: let newlib grow/shrink it in place when it can
    uint8_t *raw = (uint8_t *)p - FA_HDR;
    hdr_of(p)->check = 0;
    uint8_t *nr = realloc(raw, size + FA_HDR);
    if (!nr) { hdr_set(p, info); return NULL; }
    void *np = nr + FA_HDR;
    hdr_set(np, FA_BIG | FA_HDR);
    return np;
  }
  void *np = fa_malloc(size);
  if (!np) return NULL;
  memcpy(np, p, old < size ? old : size);
  fa_free(p);
  return np;
}

int fa_posix_memalign(void **out, size_t align, size_t size) {
  if (!out || align < sizeof(void *) || (align & (align - 1))) return EINVAL;
  void *p = (align <= FA_GRAIN) ? fa_malloc(size) : big_alloc(size, align);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}
