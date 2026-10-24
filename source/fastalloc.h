/* fastalloc.h -- thread-caching allocator for the engine's malloc imports.
 * MIT license; see LICENSE. */

#ifndef __FASTALLOC_H__
#define __FASTALLOC_H__

#include <stddef.h>

void *fa_malloc(size_t size);
void  fa_free(void *p);
void *fa_calloc(size_t n, size_t size);
void *fa_realloc(void *p, size_t size);
int   fa_posix_memalign(void **out, size_t align, size_t size);

#endif
