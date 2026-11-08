/* Ark C Compiler - memory
 * Bump allocator implementation + simple utility functions.
 * Note: all allocations are released at once via mem_pool_reset after compilation ends.
 */
#include "memory.h"
#include "platform.h"

/* Place the large pool in .ark_buf section so the linker script can put it
 * in AXI SRAM (0x24000000) on STM32H743. On host builds this is a no-op. */
static unsigned char g_pool[ARK_POOL_SIZE] __attribute__((section(".ark_buf")));
static int           g_offset = 0;
/* High-water mark of the bump pointer.  The board budget is set from this
 * number (see the in-chip ARKC_DEFS in the ARKRTOS_TEST Makefile): "the pool
 * ran out at N bytes" is what a failed build tells you, but a successful one
 * only tells you it was under the limit.  Reported by -v. */
static int           g_high = 0;
/* Bumped by mem_pool_reset(); see mem_pool_generation() in memory.h. */
static int           g_generation = 0;

void mem_pool_reset(void) {
    g_offset = 0;
    g_high = 0;
    g_generation++;
}

int mem_pool_generation(void) {
    return g_generation;
}

int mem_pool_used(void) {
    return g_offset;
}

int mem_pool_high(void) {
    return g_high;
}

int mem_pool_capacity(void) {
    return ARK_POOL_SIZE;
}

void *mem_pool_alloc(int size) {
    if (size <= 0) size = 1;
    size = (size + 7) & ~7; /* 8-byte aligned, compatible with 64-bit host pointers */
    if (g_offset + size > ARK_POOL_SIZE) {
        ark_abort("memory pool exhausted");
        return 0;
    }
    void *p = &g_pool[g_offset];
    g_offset += size;
    if (g_offset > g_high) g_high = g_offset;
    /* Zero fill */
    {
        unsigned char *q = (unsigned char *)p;
        int i;
        for (i = 0; i < size; i++) q[i] = 0;
    }
    return p;
}

void mem_pool_free(void *ptr, int size) {
    /* Bump allocator: only the most recent allocation can be reclaimed, by
     * rolling back the bump pointer. Freeing an older allocation is a no-op
     * (other allocations sit above it); this mirrors mem_realloc above, which
     * also only adjusts the last allocation in place. The freed region is
     * zero-filled again by the next mem_pool_alloc that reuses it, so we do
     * not zero it here. */
    unsigned char *p = (unsigned char *)ptr;
    int aligned;
    if (!p || size <= 0) return;
    aligned = (size + 7) & ~7;
    if (p + aligned == &g_pool[g_offset]) {
        g_offset -= aligned;
    }
    /* Otherwise: older allocation, cannot reclaim. Silent no-op. */
}

int ark_strlen(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int ark_streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

char *mem_strdup(const char *s) {
    int n;
    char *d;
    int i;
    if (!s) return 0;
    n = ark_strlen(s);
    d = (char *)mem_pool_alloc(n + 1);
    for (i = 0; i <= n; i++) d[i] = s[i];
    return d;
}

char *mem_strndup(const char *s, int n) {
    char *d;
    int i;
    if (!s) return 0;
    d = (char *)mem_pool_alloc(n + 1);
    for (i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
    return d;
}

void *mem_realloc(void *old_ptr, int old_size, int new_size) {
    /* Bump-allocator in-place extension: if old_ptr is the most recent
     * allocation, we can extend (or shrink) it without copying. This turns
     * repeated single-element growth (e.g. parse_program's decls array,
     * block statement lists, struct field lists) from O(N^2) pool usage
     * into O(N), which previously exhausted the 1 MB pool on ~500-decl files.
     * old_size is the logical size the caller passed previously; mem_pool_alloc
     * rounds up to 8 bytes, so we compare against the aligned old size. */
    if (old_ptr && old_size > 0 && new_size > 0) {
        int aligned_old = (old_size + 7) & ~7;
        int aligned_new = (new_size + 7) & ~7;
        unsigned char *end = (unsigned char *)old_ptr + aligned_old;
        if (end == &g_pool[g_offset]) {
            /* Last allocation: adjust the bump pointer in place */
            int delta = aligned_new - aligned_old;
            if (g_offset + delta <= ARK_POOL_SIZE) {
                g_offset += delta;
                /* Zero newly exposed bytes (shrinking leaves them, which is fine) */
                if (delta > 0) {
                    unsigned char *q = end;
                    int i;
                    for (i = 0; i < delta; i++) q[i] = 0;
                }
                return old_ptr;
            }
        }
    }
    /* Fallback: allocate fresh storage and copy. The old allocation remains
     * in the pool (bump allocator cannot reclaim), so callers should prefer
     * the in-place path above by growing the same array repeatedly. */
    {
        void *p = mem_pool_alloc(new_size);
        int copy = old_size < new_size ? old_size : new_size;
        int i;
        unsigned char *src = (unsigned char *)old_ptr;
        unsigned char *dst = (unsigned char *)p;
        if (old_ptr && old_size > 0) {
            for (i = 0; i < copy; i++) dst[i] = src[i];
        }
        return p;
    }
}
