#ifndef TS_HEAP_H
#define TS_HEAP_H

#include "page.h"
#include "size_class.h"

#include <pthread.h>
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
 * lists; it pushes the block onto the owner's `remote_free` instead, and
 * the owner takes the whole list back the next time it runs out of blocks.
 */
typedef struct {
    ts_page_t *head;
} ts_page_queue_t;

struct ts_heap_s {
    ts_page_queue_t pages[TS_NUM_CLASSES];
    ts_segment_t *segments; /* every segment this heap owns */

    /* Blocks freed by other threads. Pushed and taken under remote_lock;
     * atomic only so the owner can peek without the lock to see whether
     * there is anything to take. */
    pthread_mutex_t remote_lock;
    _Atomic(ts_block_t *) remote_free;
};

/* Set up an empty heap. */
void ts_heap_init(ts_heap_t *heap);

/* Block of at least `size` bytes (size <= TS_SMALL_MAX), or NULL with errno
 * set when the OS is out of memory. */
void *ts_heap_malloc_small(ts_heap_t *heap, size_t size);

/* Block of exactly ts_class_size(cls) bytes. */
void *ts_heap_malloc_class(ts_heap_t *heap, unsigned cls);

/* Return a block from ts_heap_malloc_small. `page` is its page descriptor.
 * Only the heap's owner may call this. */
void ts_heap_free_small(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page, void *p);

/* Return a block of `heap` from any thread other than its owner. */
void ts_heap_free_remote(ts_heap_t *heap, void *p);

/* Owner only: take back every block other threads have freed so far.
 * Returns how many there were. ts_heap_malloc_class calls this itself
 * before reaching for a fresh page. */
size_t ts_heap_collect_remote(ts_heap_t *heap);

#endif /* TS_HEAP_H */
