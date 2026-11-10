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
#include "registry.h"
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

/* ---- fork ---------------------------------------------------------------- */

int ts_fork_via_zone;

void ts_fork_prepare(void) {
    ts_heap_fork_prepare();
    ts_huge_fork_prepare();
}

void ts_fork_parent(void) {
    ts_huge_fork_parent();
    ts_heap_fork_parent();
}

void ts_fork_child(void) {
    ts_huge_fork_child();
    ts_heap_fork_child();
}

static void atfork_prepare(void) {
    if (!ts_fork_via_zone)
        ts_fork_prepare();
}

static void atfork_parent(void) {
    if (!ts_fork_via_zone)
        ts_fork_parent();
}

static void atfork_child(void) {
    if (!ts_fork_via_zone)
        ts_fork_child();
}

/* From a constructor, not the first malloc: pthread_atfork can itself malloc,
 * and inside heap_create that would recurse into pthread_once. Creating the
 * key here also means no fork can land halfway through that pthread_once.
 *
 * Handlers of programs registered later run before ours in prepare and after
 * ours in the child, so they can use malloc from either. */
__attribute__((constructor)) static void fork_handlers_install(void) {
    pthread_once(&heap_key_once, heap_key_create);
    (void)pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
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

/* Bytes the block at p really has: its size class, or the rest of its
 * mapping. ts_usable_size is this minus the debug tail. */
static size_t block_bytes(const void *p) {
    void *region = region_of(p);
    uint32_t kind = region_kind(region);
    if (kind == TS_KIND_SEGMENT)
        return ts_ptr_page(region, p)->block_size;
    if (kind != TS_KIND_LARGE)
        bad_pointer();
    return ts_huge_usable(region);
}

/* ---- TS_DEBUG ------------------------------------------------------------
 *
 * Built with TS_DEBUG, every block keeps its last TS_DEBUG_TAIL bytes for a
 * canary: the block's address xor one constant while it is allocated, xor
 * another once it is freed. Requests grow by that much, so the canary sits
 * right past what ts_usable_size reports and a write beyond it lands there.
 *
 * free (and realloc, and ts_usable_size) first check that the pointer is
 * ours at all (the registry), then that it is the start of a block in a
 * page in use, then the canary. A freed canary means a double free; any
 * other value means something wrote past the block. Freed heap blocks are
 * filled with 0xDF so that use after free reads obvious garbage.
 *
 * What it can't see: a stale pointer freed after its block was handed out
 * again looks like a normal free, and two threads freeing the same block at
 * the same moment race on the canary. Huge blocks get the checks but not
 * the fill, since most of them are unmapped right away. */
#ifndef TS_DEBUG
#define TS_DEBUG 0
#endif

#if TS_DEBUG
#define TS_DEBUG_TAIL sizeof(uintptr_t)
#define TS_DEBUG_FILL 0xDF
#define TS_CANARY_LIVE  ((uintptr_t)0x5afeb10c4a11c0deull)
#define TS_CANARY_FREED ((uintptr_t)0xdeadf4eed0bb1e00ull)

static TS_NOINLINE void debug_fatal(const char *what, const void *p) {
    char buf[128];
    size_t n = 0;
    const char *prefix = "tsmalloc: ";
    while (*prefix)
        buf[n++] = *prefix++;
    while (*what && n < sizeof buf - 24)
        buf[n++] = *what++;
    buf[n++] = ' ';
    buf[n++] = '0';
    buf[n++] = 'x';
    uintptr_t v = (uintptr_t)p;
    for (int shift = (int)sizeof v * 8 - 4; shift >= 0; shift -= 4)
        buf[n++] = "0123456789abcdef"[(v >> shift) & 15];
    buf[n++] = '\n';
    buf[n] = '\0';
    fatal(buf);
}

TS_INLINE uintptr_t *debug_canary_at(void *p, size_t bytes) {
    return (uintptr_t *)((char *)p + bytes - TS_DEBUG_TAIL);
}

/* Is p the start of a block that free could have handed out? Doesn't look
 * at the block itself. Returns its size in bytes. */
static size_t debug_validate(const void *p) {
    if ((uintptr_t)p % TS_ALIGN != 0 || !ts_is_ours(p))
        debug_fatal("pointer was not allocated by tsmalloc:", p);
    void *region = region_of(p);
    /* ts_is_ours already pinned a huge block to its one valid address. */
    if (region_kind(region) == TS_KIND_LARGE)
        return ts_huge_usable(region);
    ts_segment_t *seg = region;
    ts_page_t *page = ts_ptr_page(seg, p);
    if (page->in_use != TS_PAGE_USED)
        debug_fatal("pointer to memory not in use (double free?):", p);
    size_t offset = (size_t)((const uint8_t *)p - ts_page_start(seg, page));
    if ((const uint8_t *)p < ts_page_start(seg, page) || offset % page->block_size != 0)
        debug_fatal("pointer into the middle of a block:", p);
    /* reserved moves as the owner carves out blocks, so only the owner may
     * read it; other threads skip this check. */
    if (atomic_load_explicit(&seg->thread_id, memory_order_relaxed) == ts_thread_id() &&
        offset / page->block_size >= page->reserved)
        debug_fatal("pointer to a block never allocated:", p);
    return page->block_size;
}

/* Validate p and its canary; returns the block's size in bytes. */
static size_t debug_check_live(void *p) {
    size_t bytes = debug_validate(p);
    uintptr_t canary = *debug_canary_at(p, bytes);
    if (canary == ((uintptr_t)p ^ TS_CANARY_FREED))
        debug_fatal("double free of", p);
    if (canary != ((uintptr_t)p ^ TS_CANARY_LIVE))
        debug_fatal("write past the end of block", p);
    return bytes;
}

TS_INLINE size_t debug_size(size_t size) {
    return size > SIZE_MAX - TS_DEBUG_TAIL ? SIZE_MAX : size + TS_DEBUG_TAIL;
}

/* Every pointer on its way out of an allocation call goes through here. */
static void *debug_alloc(void *p) {
    if (p != NULL)
        *debug_canary_at(p, block_bytes(p)) = (uintptr_t)p ^ TS_CANARY_LIVE;
    return p;
}

/* On the way into free, before the block goes back on any list (which
 * writes its first word). */
static void debug_free(void *p) {
    size_t bytes = debug_check_live(p);
    if (region_kind(region_of(p)) == TS_KIND_SEGMENT)
        memset(p, TS_DEBUG_FILL, bytes - TS_DEBUG_TAIL);
    *debug_canary_at(p, bytes) = (uintptr_t)p ^ TS_CANARY_FREED;
}
#else
#define TS_DEBUG_TAIL ((size_t)0)

TS_INLINE size_t debug_size(size_t size) {
    return size;
}

TS_INLINE void *debug_alloc(void *p) {
    return p;
}

TS_INLINE void debug_free(void *p) {
    (void)p;
}

TS_INLINE size_t debug_check_live(void *p) {
    (void)p;
    return 0;
}
#endif

void *ts_malloc(size_t size) {
    TS_STAT_INC(TS_STAT_MALLOC);
    size = debug_size(size);
    if (ts_likely(size <= TS_MEDIUM_MAX))
        return debug_alloc(small_alloc(size));
    return debug_alloc(ts_huge_alloc(size, TS_ALIGN));
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
    if (TS_DEBUG)
        debug_free(p);
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
    if (TS_DEBUG)
        debug_check_live((void *)p);
    return block_bytes(p) - TS_DEBUG_TAIL;
}

void *ts_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    TS_STAT_INC(TS_STAT_MALLOC);
    size_t n = debug_size(total);
    if (n > TS_MEDIUM_MAX)
        return debug_alloc(ts_huge_alloc_zeroed(n, TS_ALIGN));
    void *p = small_alloc(n);
    /* Heap blocks are recycled, and even never-used ones have a free-list
     * link written into their first word, so they always need clearing. */
    if (p != NULL)
        memset(p, 0, total);
    return debug_alloc(p);
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
    if (TS_DEBUG)
        debug_check_live(p);
    void *region = region_of(p);
    uint32_t kind = region_kind(region);

    /* `old` is what the caller may use; the block keeps its debug tail
     * whether it stays or moves, so sizes are compared without it. */
    if (ts_likely(kind == TS_KIND_SEGMENT)) {
        ts_page_t *page = ts_ptr_page(region, p);
        size_t old = page->block_size - TS_DEBUG_TAIL;
        /* Same block when the size stays in its class, or still fits with
         * at least half of the block in use (saves a copy for modest
         * shrinks). A big shrink moves so the rest isn't stranded. */
        if (size <= old &&
            (size >= old / 2 || ts_size_to_class(debug_size(size)) == page->size_class))
            return p;
        return move_block(p, old, size);
    }
    if (kind != TS_KIND_LARGE)
        bad_pointer();

    ts_huge_t *h = region;
    size_t old = ts_huge_usable(h) - TS_DEBUG_TAIL;
    if (size <= old && debug_size(size) > TS_MEDIUM_MAX) {
        ts_huge_shrink(h, p, debug_size(size));
        return debug_alloc(p); /* the end moved, and the canary with it */
    }
    return move_block(p, old, size);
}

/* `alignment` is a power of two. */
static void *aligned_alloc_checked(size_t alignment, size_t size) {
    if (alignment <= TS_ALIGN)
        return ts_malloc(size);
    TS_STAT_INC(TS_STAT_MALLOC);
    size = debug_size(size);
    if (alignment <= TS_PAGE0_OFFSET) {
        unsigned cls = ts_size_to_class_aligned(size, alignment);
        if (cls < TS_NUM_CLASSES)
            return debug_alloc(small_alloc_class(cls));
    }
    /* Bigger alignments, or sizes past the heap classes: a mapping of its
     * own, over-allocated by the alignment so the block can slide up. */
    return debug_alloc(ts_huge_alloc(size, alignment));
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
