/*
 * Placeholder implementation: every call forwards to the system allocator.
 * This lets the tests and benchmarks be written against the ts_ API now;
 * the real allocator replaces these one size range at a time.
 */
#if defined(__linux__)
#define _GNU_SOURCE /* malloc_usable_size, posix_memalign under -std=c11 */
#endif

#include "tsmalloc.h"

#include <errno.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

void *ts_malloc(size_t size) {
    return malloc(size);
}

void ts_free(void *p) {
    free(p);
}

void *ts_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

void *ts_realloc(void *p, size_t size) {
    return realloc(p, size);
}

int ts_posix_memalign(void **out, size_t alignment, size_t size) {
    /* Validate here rather than relying on the libc: the contract is ours,
     * and sanitizer runtimes abort on bad alignments instead of returning
     * EINVAL. */
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;
    return posix_memalign(out, alignment, size);
}

void *ts_aligned_alloc(size_t alignment, size_t size) {
    /* Go through posix_memalign: some libcs reject aligned_alloc sizes that
     * aren't a multiple of the alignment, and we want the same behaviour on
     * every platform. */
    void *p = NULL;
    int err = ts_posix_memalign(&p, alignment, size);
    if (err != 0) {
        errno = err;
        return NULL;
    }
    return p;
}

size_t ts_usable_size(const void *p) {
    if (p == NULL)
        return 0;
#if defined(__APPLE__)
    return malloc_size(p);
#else
    return malloc_usable_size((void *)p);
#endif
}
