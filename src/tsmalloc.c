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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t kind;   /* TS_KIND_LARGE */
    uint32_t lead;   /* user pointer - start of the mapping */
    size_t   mapped; /* bytes mapped, a multiple of the OS page size */
} ts_large_t;

_Static_assert(sizeof(ts_large_t) == TS_ALIGN, "large header keeps user pointers aligned");

/* One heap shared by every thread, behind one lock. This is the simple
 * design the thread-local heaps will be measured against; large blocks
 * only touch mmap/munmap and stay outside the lock. */
static ts_heap_t global_heap;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

static void *small_alloc_class(unsigned cls) {
    pthread_mutex_lock(&global_lock);
    void *p = ts_heap_malloc_class(&global_heap, cls);
    pthread_mutex_unlock(&global_lock);
    return p;
}

TS_INLINE void *small_alloc(size_t size) {
    return small_alloc_class(ts_size_to_class(size));
}

static void small_free(ts_segment_t *seg, void *p) {
    pthread_mutex_lock(&global_lock);
    ts_heap_free_small(&global_heap, seg, ts_ptr_page(seg, p), p);
    pthread_mutex_unlock(&global_lock);
}

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
        return small_alloc(size);
    return large_alloc(size, TS_ALIGN);
}

void ts_free(void *p) {
    if (p == NULL)
        return;
    void *region = region_of(p);
    uint32_t kind = region_kind(region);
    if (ts_likely(kind == TS_KIND_SEGMENT)) {
        small_free(region, p);
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
    if (total > TS_SMALL_MAX)
        return large_alloc(total, TS_ALIGN); /* fresh mmap memory is zero */
    void *p = small_alloc(total);
    /* Small blocks are recycled, and even never-used ones have a free-list
     * link written into their first word, so they always need clearing. */
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

/* Give back the whole OS pages of a large mapping beyond the first `size`
 * bytes, keeping the block where it is. */
static void large_shrink(ts_large_t *h, void *p, size_t size) {
    size_t keep = ts_align_up(h->lead + size, ts_os_page_size());
    if (keep < h->mapped) {
        char *base = (char *)p - h->lead;
        ts_os_free(base + keep, h->mapped - keep);
        h->mapped = keep;
    }
}

static void *move_block(void *p, size_t old, size_t size) {
    void *q = ts_malloc(size);
    if (q == NULL)
        return NULL; /* p stays valid, as the standard requires */
    memcpy(q, p, old < size ? old : size);
    ts_free(p);
    return q;
}

void *ts_realloc(void *p, size_t size) {
    if (p == NULL)
        return ts_malloc(size);
    void *region = region_of(p);
    uint32_t kind = region_kind(region);

    if (ts_likely(kind == TS_KIND_SEGMENT)) {
        ts_page_t *page = ts_ptr_page(region, p);
        size_t old = page->block_size;
        /* Same block when the size stays in its class, or still fits with
         * at least half of the block in use (saves a copy for modest
         * shrinks). A big shrink moves so the rest isn't stranded. */
        if (size <= old && (size >= old / 2 || ts_size_to_class(size) == page->size_class))
            return p;
        return move_block(p, old, size);
    }
    if (kind != TS_KIND_LARGE)
        bad_pointer();

    ts_large_t *h = region;
    size_t old = large_usable(h);
    if (size <= old && size > TS_SMALL_MAX) {
        large_shrink(h, p, size);
        return p;
    }
    return move_block(p, old, size);
}

/* `alignment` is a power of two. */
static void *aligned_alloc_checked(size_t alignment, size_t size) {
    if (alignment <= TS_ALIGN)
        return ts_malloc(size);
    if (alignment <= TS_PAGE0_OFFSET) {
        unsigned cls = ts_size_to_class_aligned(size, alignment);
        if (cls < TS_NUM_CLASSES)
            return small_alloc_class(cls);
    }
    /* Bigger alignments, or sizes past the small classes: a mapping of its
     * own, over-allocated by the alignment so the block can slide up. */
    return large_alloc(size, alignment);
}

int ts_posix_memalign(void **out, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || !ts_is_pow2(alignment))
        return EINVAL;
    void *p = aligned_alloc_checked(alignment, size);
    if (p == NULL)
        return ENOMEM;
    *out = p;
    return 0;
}

/* Unlike posix_memalign, any power of two is a valid alignment here,
 * including ones smaller than a pointer. The size doesn't have to be a
 * multiple of the alignment (C17 dropped that requirement). */
void *ts_aligned_alloc(size_t alignment, size_t size) {
    if (!ts_is_pow2(alignment)) {
        errno = EINVAL;
        return NULL;
    }
    return aligned_alloc_checked(alignment, size);
}
