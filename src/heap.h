#ifndef TS_HEAP_H
#define TS_HEAP_H

#include "page.h"
#include "size_class.h"

#include <stdatomic.h>

/*
 * A heap owns segments and hands out small blocks from their pages.
 *
 * For each size class it keeps a queue of pages that may still have free
 * blocks. A page that turns out to be full is unlinked the next time malloc
 * looks at it, so the queue head is almost always allocatable; the first
 * free into a full page links it back in at the front.
 *
 * Each thread has its own heap (see tsmalloc.c) and is the only one that
 * touches its page queues and free lists, so none of that is locked. A
 * thread freeing a block from another thread's heap can't touch those
 * lists. It pushes the block onto its page's atomic `thread_free` list
 * instead, and the owner splices that in when the page runs dry.
 *
 * That leaves pages that already ran dry and left their queue: the owner
 * doesn't look at those. Such a page carries TS_TF_FULL, and the first
 * remote free into it goes onto the heap's `delayed_free` list instead.
 * When the owner runs out of blocks it frees that list locally, which puts
 * those pages back on their queues. Later remote frees into them go to
 * `thread_free` as usual.
 *
 * When the owning thread exits, its heap is abandoned: everything that can
 * be freed is, and segments that still hold live blocks go onto a global
 * abandoned list with owner 0. Frees into them keep landing on their pages'
 * `thread_free` lists until another thread adopts them: a heap that is
 * about to need a fresh page takes one abandoned segment first, collects
 * what was freed into it and queues its pages as its own.
 */
typedef struct {
    ts_page_t *head;
} ts_page_queue_t;

/* Empty segments a heap keeps rather than unmapping. A program whose live
 * memory swings by a few segments shouldn't pay for an munmap and an mmap
 * (plus page faults) every swing; past this, empty segments go back. */
#define TS_SEGMENT_CACHE_MAX 2

struct ts_heap_s {
    uintptr_t thread_id;    /* owner's ts_thread_id(), 0 for a heap no thread owns */
    ts_page_queue_t pages[TS_NUM_CLASSES];
    /* At most one empty page per class stays queued instead of going back
     * to its segment; this is it, if it is still empty. */
    ts_page_t *spare[TS_NUM_CLASSES];
    ts_segment_t *segments;  /* segments with pages in use */
    ts_segment_t *seg_cache; /* empty segments kept for reuse (via next) */
    unsigned seg_cache_count;

    /* Remote frees into full pages (see above). Many pushers, and only the
     * owner takes, always the whole list at once, so there is no ABA. */
    _Atomic(ts_block_t *) delayed_free;
};

/* Set up an empty heap with no owning thread. */
void ts_heap_init(ts_heap_t *heap);

/* Block of at least `size` bytes (size <= TS_SMALL_MAX), or NULL with errno
 * set when the OS is out of memory. */
void *ts_heap_malloc_small(ts_heap_t *heap, size_t size);

/* Block of exactly ts_class_size(cls) bytes. */
void *ts_heap_malloc_class(ts_heap_t *heap, unsigned cls);

/* Return a block from ts_heap_malloc_small. `page` is its page descriptor.
 * Only the heap's owner may call this. */
void ts_heap_free_small(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page, void *p);

/* Return a block of `seg` from any thread other than its owner (or when it
 * has none). Lock-free, except for waiting out another freer that is busy
 * handing a block of the same page to the heap. */
void ts_heap_free_remote(ts_segment_t *seg, ts_page_t *page, void *p);

/* Owner only: free the blocks on `delayed_free`, re-queueing their pages.
 * Returns how many there were. ts_heap_malloc_class calls this itself
 * before reaching for a fresh page. */
size_t ts_heap_collect_remote(ts_heap_t *heap);

/* Owner only, as its thread exits: give everything back that can be given
 * back and abandon the rest. The heap is empty afterwards (and no other
 * thread will touch it again, so it may be unmapped). */
void ts_heap_abandon(ts_heap_t *heap);

/* Owner only: take over one abandoned segment, if there is any. Returns 1
 * if it did. ts_heap_malloc_class calls this before mapping new memory;
 * heaps without an owning thread never adopt. */
int ts_heap_adopt(ts_heap_t *heap);

/* Number of abandoned segments, and a way for tests to look at them. The
 * callback runs with the abandoned list locked. */
size_t ts_abandoned_count(void);
void ts_abandoned_visit(void (*fn)(ts_segment_t *seg, void *arg), void *arg);

#endif /* TS_HEAP_H */
