/*
 * Replaces the libc allocator on Linux by defining the same symbols, either
 * from libtsmalloc.so (LD_PRELOAD it, or link against it) or from the static
 * libtsmalloc_override.a. The dynamic linker binds every image's malloc to
 * the first definition it finds, so glibc's own internal calls land here too.
 *
 * This can run before libc has finished starting up, so nothing here (or
 * below it) may depend on malloc or on stdio.
 *
 * With LD_PRELOAD we are in place before the first allocation, so a pointer
 * we didn't make is close to impossible: the loader keeps track of its
 * early bootstrap blocks itself. If one does turn up there is no safe way to
 * give it back. Calling glibc's __libc_free would crash on a loader block and
 * would pull glibc's malloc into static links, so free drops it (a small
 * leak) and realloc, which can't know how much to copy, reports failure.
 */
#define _GNU_SOURCE

#include "internal.h"
#include "os.h"
#include "registry.h"
#include "tsmalloc.h"

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

void *malloc(size_t size) {
    return ts_malloc(size);
}

void free(void *p) {
    if (ts_is_ours(p))
        ts_free(p);
}

void *calloc(size_t count, size_t size) {
    return ts_calloc(count, size);
}

void *realloc(void *p, size_t size) {
    if (p == NULL)
        return ts_malloc(size);
    if (!ts_is_ours(p)) {
        errno = ENOMEM;
        return NULL;
    }
    /* ts_realloc returns a minimal block here, but programs written against
     * glibc expect free-and-return-NULL and often drop the result. */
    if (size == 0) {
        ts_free(p);
        return NULL;
    }
    return ts_realloc(p, size);
}

void *reallocarray(void *p, size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, total);
}

void *aligned_alloc(size_t alignment, size_t size) {
    return ts_aligned_alloc(alignment, size);
}

int posix_memalign(void **out, size_t alignment, size_t size) {
    return ts_posix_memalign(out, alignment, size);
}

/* The old interface takes anything: like glibc, an alignment that isn't a
 * power of two is rounded up to one. */
void *memalign(size_t alignment, size_t size) {
    if (alignment > (SIZE_MAX >> 1) + 1) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment <= 1)
        return ts_malloc(size);
    if (!ts_is_pow2(alignment))
        alignment = (size_t)1 << (64 - __builtin_clzll((unsigned long long)alignment - 1));
    return ts_aligned_alloc(alignment, size);
}

void *valloc(size_t size) {
    return ts_aligned_alloc(ts_os_page_size(), size);
}

/* valloc, with the size rounded up to whole pages (0 becomes one page). */
void *pvalloc(size_t size) {
    size_t page = ts_os_page_size();
    if (size > SIZE_MAX - (page - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t rounded = size == 0 ? page : (size + page - 1) & ~(page - 1);
    return ts_aligned_alloc(page, rounded);
}

size_t malloc_usable_size(void *p) {
    return ts_is_ours(p) ? ts_usable_size(p) : 0;
}
