/*
 * Public entry points.
 *
 * Every pointer we hand out is found from its address alone:
 *
 *   - small and medium (<= TS_MEDIUM_MAX): a block in a page, or a span of
 *     pages, of a 4 MiB segment, with no per-block header. Masking the
 *     pointer gives the segment, and the segment's page descriptors give
 *     the block size.
 *   - huge: its own mapping, aligned to the segment size, with a 16-byte
 *     ts_huge_t at the start (see huge.c). Masking the pointer finds that too.
 *
 * Both start with a `kind` word, so ts_free does one mask and one load to
 * route the pointer. We mask `p - 1` rather than `p` so a large block with
 * an alignment of 4 MiB or more (whose user pointer is itself segment
 * aligned) can keep its header in the segment just before it; small blocks
 * never sit at offset 0 of a segment, so for them it makes no difference.
 */
#include "tsmalloc.h"

#include "heap.h"
#include "huge.h"
#include "internal.h"
#include "os.h"
#include "stats.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The calling thread's heap, created on its first small malloc. Its memory
 * comes straight from the OS layer: allocating it with malloc would recurse
 * once tsmalloc is malloc.
 *
 * The heap is also stored under a pthread key, whose destructor abandons it
 * when the thread exits (see ts_heap_abandon). */
static pthread_key_t heap_key;
static pthread_once_t heap_key_once = PTHREAD_ONCE_INIT;
static int heap_key_ok;

#if defined(__APPLE__) && defined(__aarch64__)
/* dyld can malloc the first time a thread touches a _Thread_local, which is
 * a recursion once we are malloc. So on macOS the heap lives only in the
 * key's slot, and is read straight out of the thread's TSD array the same
 * way pthread_getspecific would: slot number = key, array as in ts_thread_id.
 * Dynamic keys start at 256, so 0 can mean "no key yet". */
static _Atomic size_t heap_slot;

TS_INLINE ts_heap_t *heap_current(void) {
    size_t slot = atomic_load_explicit(&heap_slot, memory_order_relaxed);
    if (ts_unlikely(slot == 0))
        return NULL;
    uintptr_t tsd;
    __asm__("mrs %0, tpidrro_el0" : "=r"(tsd));
    return ((ts_heap_t *const *)(tsd & ~(uintptr_t)7))[slot];
}

TS_INLINE void heap_set_current(ts_heap_t *heap) {
    (void)heap; /* pthread_setspecific in heap_create is the only store */
}

static void heap_key_published(void) {
    atomic_store_explicit(&heap_slot, (size_t)heap_key, memory_order_relaxed);
}
#else
/* initial-exec: the default model in a shared library goes through
 * __tls_get_addr, which can malloc, and that is us. It's also one load less. */
__attribute__((tls_model("initial-exec"))) static _Thread_local ts_heap_t *tl_heap;

TS_INLINE ts_heap_t *heap_current(void) {
    return tl_heap;
}

TS_INLINE void heap_set_current(ts_heap_t *heap) {
    tl_heap = heap;
}

static void heap_key_published(void) {}
#endif

static void heap_thread_exit(void *arg) {
    ts_heap_t *heap = arg;
    /* Other destructors may still run after us and malloc or free. Their
     * mallocs get a fresh heap (and a fresh key value, so we're called
     * again); their frees of our blocks see owner 0 and go remote. */
    heap_set_current(NULL);
    ts_heap_abandon(heap);
    ts_os_free(heap, sizeof *heap);
}

static void heap_key_create(void) {
    heap_key_ok = pthread_key_create(&heap_key, heap_thread_exit) == 0;
    if (heap_key_ok)
        heap_key_published();
}

static TS_NOINLINE ts_heap_t *heap_create(void) {
    ts_heap_t *heap = ts_os_alloc(sizeof *heap);
    if (heap == NULL)
        return NULL; /* errno is ENOMEM */
    ts_heap_init(heap);
    heap->thread_id = ts_thread_id();
    heap_set_current(heap);
    /* If the key can't be set the heap just outlives its thread, as all
     * heaps used to: a leak, not a crash. */
    pthread_once(&heap_key_once, heap_key_create);
    if (heap_key_ok)
        (void)pthread_setspecific(heap_key, heap);
    return heap;
}

static TS_NOINLINE void *small_alloc_class(unsigned cls) {
    ts_heap_t *heap = heap_current();
    if (ts_unlikely(heap == NULL)) {
        heap = heap_create();
        if (heap == NULL)
            return NULL;
    }
    return ts_heap_malloc_class(heap, cls);
}

/* The malloc fast path, inlined into every caller: for sizes up to
 * TS_LUT_MAX it is the TLS load, one load from pages_direct, and popping
 * the page's free list. Anything else (no heap yet, an empty or exhausted
 * page, bigger sizes) goes through small_alloc_class, which never inlines
 * so this stays small. */
TS_INLINE void *small_alloc(size_t size) {
    ts_heap_t *heap = heap_current();
    if (ts_likely(heap != NULL && size <= TS_LUT_MAX)) {
        ts_page_t *page = heap->pages_direct[(size + 15) >> 4];
        ts_block_t *b = page->free;
        if (ts_likely(b != NULL)) {
            page->free = b->next;
            page->used++;
            return b;
        }
    }
    return small_alloc_class(ts_size_to_class(size));
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

static TS_NOINLINE void bad_pointer(void) {
    fatal("tsmalloc: free/realloc of a pointer not from tsmalloc\n");
}

void *ts_malloc(size_t size) {
    TS_STAT_INC(TS_STAT_MALLOC);
    if (ts_likely(size <= TS_MEDIUM_MAX))
        return small_alloc(size);
    return ts_huge_alloc(size, TS_ALIGN);
}

/* Everything ts_free doesn't finish inline: remote small frees, local ones
 * that empty a page or bring it back to its queue, and large blocks. */
static TS_NOINLINE void free_slow(void *region, void *p) {
    uint32_t kind = region_kind(region);
    if (ts_likely(kind == TS_KIND_SEGMENT)) {
        ts_heap_free_remote(region, p);
    } else if (kind == TS_KIND_LARGE) {
        ts_huge_free(region, p);
    } else {
        bad_pointer();
    }
}

/* Local or remote is decided by the thread id stored in the segment rather
 * than by comparing heaps: the id is one register read away, and finding
 * the heap can take a call into the TLS machinery on some platforms. A thread that
 * never allocated owns no segment, so its frees all take the remote path,
 * as they should. Relaxed is enough: only this thread ever stores its own
 * id into a segment, so the comparison can't succeed spuriously.
 *
 * A large mapping's header has no thread id where a segment keeps it
 * (offset 8 is its `mapped` size, never a valid thread pointer), but the
 * kind check comes first anyway so that is never relied on. */
void ts_free(void *p) {
    if (ts_unlikely(p == NULL))
        return;
    TS_STAT_INC(TS_STAT_FREE);
    ts_segment_t *seg = region_of(p);
    if (ts_likely(seg->kind == TS_KIND_SEGMENT &&
                  atomic_load_explicit(&seg->thread_id, memory_order_relaxed) == ts_thread_id())) {
        ts_page_t *page = ts_ptr_page(seg, p);
        ts_block_t *b = p;
        b->next = page->local_free;
        page->local_free = b;
        /* The common case ends here: the page still has live blocks and is
         * on its queue, so there is nothing to fix up. */
        if (ts_likely(--page->used != 0 && page->queued))
            return;
        ts_heap_page_freed(seg->heap, seg, page);
        return;
    }
    free_slow(seg, p);
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
    return ts_huge_usable(region);
}

void *ts_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    TS_STAT_INC(TS_STAT_MALLOC);
    if (total > TS_MEDIUM_MAX)
        return ts_huge_alloc_zeroed(total, TS_ALIGN);
    void *p = small_alloc(total);
    /* Heap blocks are recycled, and even never-used ones have a free-list
     * link written into their first word, so they always need clearing. */
    if (p != NULL)
        memset(p, 0, total);
    return p;
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

    ts_huge_t *h = region;
    size_t old = ts_huge_usable(h);
    if (size <= old && size > TS_MEDIUM_MAX) {
        ts_huge_shrink(h, p, size);
        return p;
    }
    return move_block(p, old, size);
}

/* `alignment` is a power of two. */
static void *aligned_alloc_checked(size_t alignment, size_t size) {
    if (alignment <= TS_ALIGN)
        return ts_malloc(size);
    TS_STAT_INC(TS_STAT_MALLOC);
    if (alignment <= TS_PAGE0_OFFSET) {
        unsigned cls = ts_size_to_class_aligned(size, alignment);
        if (cls < TS_NUM_CLASSES)
            return small_alloc_class(cls);
    }
    /* Bigger alignments, or sizes past the heap classes: a mapping of its
     * own, over-allocated by the alignment so the block can slide up. */
    return ts_huge_alloc(size, alignment);
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
