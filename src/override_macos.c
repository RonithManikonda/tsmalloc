/*
 * Replaces the libc allocator in any program started with
 * DYLD_INSERT_LIBRARIES=libtsmalloc.dylib, through dyld's interpose table:
 * each entry below tells dyld to bind other images' references to the second
 * symbol to the first instead.
 *
 * An interposing image's own references aren't rewritten, so a plain call to
 * free or realloc in this file reaches the real one. That is how pointers we
 * didn't allocate are dealt with: anything allocated before the table took
 * effect, or straight from a malloc zone, is handed back to the system.
 */
#include "registry.h"
#include "tsmalloc.h"

#include <errno.h>
#include <malloc/malloc.h>
#include <stdlib.h>
#include <unistd.h>

static void *over_malloc(size_t size) {
    return ts_malloc(size);
}

static void over_free(void *p) {
    if (p == NULL)
        return;
    if (ts_is_ours(p))
        ts_free(p);
    else
        free(p);
}

static void *over_calloc(size_t count, size_t size) {
    return ts_calloc(count, size);
}

static void *over_realloc(void *p, size_t size) {
    if (p == NULL)
        return ts_malloc(size);
    if (ts_is_ours(p))
        return ts_realloc(p, size);
    return realloc(p, size);
}

/* Same as realloc, except that p is released if it fails. */
static void *over_reallocf(void *p, size_t size) {
    void *q = over_realloc(p, size);
    if (q == NULL && size != 0)
        over_free(p);
    return q;
}

static void *over_valloc(size_t size) {
    return ts_aligned_alloc((size_t)getpagesize(), size);
}

static int over_posix_memalign(void **out, size_t alignment, size_t size) {
    return ts_posix_memalign(out, alignment, size);
}

static void *over_aligned_alloc(size_t alignment, size_t size) {
    return ts_aligned_alloc(alignment, size);
}

static size_t over_malloc_size(const void *p) {
    return ts_is_ours(p) ? ts_usable_size(p) : malloc_size(p);
}

typedef struct {
    const void *replacement;
    const void *original;
} interpose_t;

#define INTERPOSE(replacement, original) \
    { (const void *)(replacement), (const void *)(original) }

__attribute__((used, section("__DATA,__interpose")))
static const interpose_t interposers[] = {
    INTERPOSE(over_malloc, malloc),
    INTERPOSE(over_free, free),
    INTERPOSE(over_calloc, calloc),
    INTERPOSE(over_realloc, realloc),
    INTERPOSE(over_reallocf, reallocf),
    INTERPOSE(over_valloc, valloc),
    INTERPOSE(over_posix_memalign, posix_memalign),
    INTERPOSE(over_aligned_alloc, aligned_alloc),
    INTERPOSE(over_malloc_size, malloc_size),
};
