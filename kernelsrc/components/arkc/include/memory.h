/* Ark C Compiler - memory.h
 * Bump allocator, used as the memory pool for AST/IR/symbol tables.
 */
#ifndef ARK_MEMORY_H
#define ARK_MEMORY_H

#include "common.h"
#include "arkcc_config.h"   /* brings in profile-aware ARK_POOL_SIZE */

/* ARK_POOL_SIZE is defined by arkcc_config.h (profile-aware):
 *   STM32 profile: 128 KB
 *   Host profile:   64 MB
 * Override via -DARK_POOL_SIZE=<bytes> on the command line if needed. */

void  mem_pool_reset(void);
void *mem_pool_alloc(int size);
/* Release an allocation. This is a bump allocator: only the most recent
 * allocation can actually be reclaimed (by rolling back the bump pointer);
 * freeing an older allocation is a silent no-op because other allocations
 * sit above it. Callers that need guaranteed reclamation must free immediately,
 * before any further mem_pool_alloc. `size` must match the size passed to the
 * corresponding mem_pool_alloc call. */
void  mem_pool_free(void *ptr, int size);
char *mem_strdup(const char *s);
char *mem_strndup(const char *s, int n);
void *mem_realloc(void *old_ptr, int old_size, int new_size);
int   mem_pool_used(void);
/* Peak of the bump pointer since the last mem_pool_reset(): how much arena the
 * translation unit actually needed.  Lets the RAM budget be set from data
 * instead of from the number in a failed build's error message. */
int   mem_pool_high(void);
int   mem_pool_capacity(void);

#endif /* ARK_MEMORY_H */
