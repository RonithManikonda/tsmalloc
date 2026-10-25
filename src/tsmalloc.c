/*
 * Public entry points.
 *
 * Every pointer we hand out is found from its address alone:
 *
 *   - small (<= TS_SMALL_MAX): a block in a page of a 4 MiB segment, with no
 *     per-block header. Masking the pointer gives the segment, and the
 *     segment's page descriptors give the block size.
 *   - large: its own mapping, aligned to the segment size, with a 16-byte
 *     ts_large_t at the start. Masking the pointer finds that too.
 *
 * Both start with a `kind` word, so ts_free does one mask and one load to
 * route the pointer. We mask `p - 1` rather than `p` so a large block with
 * an alignment of 4 MiB or more (whose user pointer is itself segment
 * aligned) can keep its header in the segment just before it; small blocks
 * never sit at offset 0 of a segment, so for them it makes no difference.
 */
#include "tsmalloc.h"

#include "heap.h"
#include "internal.h"
#include "os.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t kind;   /* TS_KIND_LARGE */
    uint32_t lead;   /* user pointer - start of the mapping */
    size_t   mapped; /* bytes mapped, a multiple of the OS page size */
} ts_large_t;

_Static_assert(sizeof(ts_large_t) == TS_ALIGN, "large header keeps user pointers aligned");

static ts_heap_t global_heap;

TS_INLINE void *region_of(const void *p) {
    return ts_ptr_segment((const char *)p - 1);
}

TS_INLINE uint32_t region_kind(const void *region) {
    return *(const uint32_t *)region;
}

static void fatal(const char *msg) {
    /* write(2) rather than stdio: stdio may allocate. */
    (void)!write(STDERR_FILENO, msg, strlen(msg));
    abort();
}

/* `size` bytes aligned to `align` (a power of two >= TS_ALIGN) in a mapping
 * of their own. */
static void *large_alloc(size_t size, size_t align) {
    size_t lead = align > sizeof(ts_large_t) ? align : sizeof(ts_large_t);
    if (align > ((size_t)1 << 31) || size > SIZE_MAX - lead - TS_SEGMENT_SIZE) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = lead + size;
    size_t map_align = align > TS_SEGMENT_SIZE ? align : TS_SEGMENT_SIZE;
    char *base = ts_os_alloc_aligned(total, map_align);
    if (base == NULL)
        return NULL;

    char *user = base + lead;
    ts_large_t *h = region_of(user);
    h->kind = TS_KIND_LARGE;
    h->lead = (uint32_t)lead;
    /* The mapping is rounded up to whole OS pages; the tail is ours to hand
     * out, which is what makes realloc-in-place possible. */
    h->mapped = ts_align_up(total, ts_os_page_size());
    return user;
}

static void large_free(ts_large_t *h, void *p) {
    ts_os_free((char *)p - h->lead, h->mapped);
}

TS_INLINE size_t large_usable(const ts_large_t *h) {
    return h->mapped - h->lead;
}

static TS_NOINLINE void bad_pointer(void) {
    fatal("tsmalloc: free/realloc of a pointer not from tsmalloc\n");
}

void *ts_malloc(size_t size) {
    if (ts_likely(size <= TS_SMALL_MAX))
        return ts_heap_malloc_small(&global_heap, size);
    return large_alloc(size, TS_ALIGN);
}

void ts_free(void *p) {
    if (p == NULL)
        return;
    void *region = region_of(p);
    uint32_t kind = region_kind(region);
    if (ts_likely(kind == TS_KIND_SEGMENT)) {
        ts_segment_t *seg = region;
        ts_heap_free_small(&global_heap, seg, ts_ptr_page(seg, p), p);
    } else if (kind == TS_KIND_LARGE) {
        large_free(region, p);
    } else {
        bad_pointer();
    }
}

size_t ts_usable_size(const void *p) {
    if (p == NULL)
        return 0;
    void *region = region_of(p);
    uint32_t kind = region_kind(region);
    if (kind == TS_KIND_SEGMENT)
        return ts_ptr_page(region, p)->block_size;
    if (kind != TS_KIND_LARGE)
        bad_pointer();
    return large_usable(region);
}

void *ts_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = ts_malloc(total);
    if (p == NULL)
        return NULL;
    /* Fresh mappings are already zero; recycled small blocks are not. */
    if (region_kind(region_of(p)) != TS_KIND_LARGE)
        memset(p, 0, total);
    return p;
}

void *ts_realloc(void *p, size_t size) {
    if (p == NULL)
        return ts_malloc(size);
    size_t old = ts_usable_size(p);
    /* Fits already and wouldn't waste more than half: keep the block. */
    if (size <= old && size >= old / 2)
        return p;
    void *q = ts_malloc(size);
    if (q == NULL)
        return NULL; /* p stays valid, as the standard requires */
    memcpy(q, p, old < size ? old : size);
    ts_free(p);
    return q;
}

int ts_posix_memalign(void **out, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || !ts_is_pow2(alignment))
        return EINVAL;
    void *p;
    if (alignment <= TS_ALIGN && size <= TS_SMALL_MAX)
        p = ts_heap_malloc_small(&global_heap, size);
    else /* over-aligned requests all take a mapping for now */
        p = large_alloc(size, alignment < TS_ALIGN ? TS_ALIGN : alignment);
    if (p == NULL)
        return ENOMEM;
    *out = p;
    return 0;
}

void *ts_aligned_alloc(size_t alignment, size_t size) {
    void *p = NULL;
    int err = ts_posix_memalign(&p, alignment, size);
    if (err != 0) {
        errno = err;
        return NULL;
    }
    return p;
}
