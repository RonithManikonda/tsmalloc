#ifndef TS_HEAP_H
#define TS_HEAP_H

#include "page.h"
#include "size_class.h"

/*
 * A heap owns segments and hands out small blocks from their pages.
 *
 * For each size class it keeps a queue of pages that may still have free
 * blocks. A page that turns out to be full is unlinked the next time malloc
 * looks at it, so the queue head is almost always allocatable; the first
 * free into a full page links it back in at the front.
 *
 * There is a single global heap for now, with no locking.
 */
typedef struct {
    ts_page_t *head;
} ts_page_queue_t;

struct ts_heap_s {
    ts_page_queue_t pages[TS_NUM_CLASSES];
    ts_segment_t *segments; /* every segment this heap owns */
};

/* Block of at least `size` bytes (size <= TS_SMALL_MAX), or NULL with errno
 * set when the OS is out of memory. */
void *ts_heap_malloc_small(ts_heap_t *heap, size_t size);

/* Block of exactly ts_class_size(cls) bytes. */
void *ts_heap_malloc_class(ts_heap_t *heap, unsigned cls);

/* Return a block from ts_heap_malloc_small. `page` is its page descriptor. */
void ts_heap_free_small(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page, void *p);

#endif /* TS_HEAP_H */
