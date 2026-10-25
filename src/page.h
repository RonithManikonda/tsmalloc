#ifndef TS_PAGE_H
#define TS_PAGE_H

#include "segment.h"

/*
 * Blocks inside one page, all of `block_size` bytes.
 *
 * A fresh page is not threaded into a free list all at once: that would
 * write to every block and fault in all 64 KiB even if the program only
 * ever uses a few. Instead `reserved` is a bump cursor, and when the free
 * list runs dry the next few KiB worth of blocks are linked in. Freed
 * blocks go back on the same list, so they are reused before the cursor
 * moves again.
 */

/* Bytes linked into the free list per extension. One OS page on Linux,
 * a quarter of one on Apple silicon; either way small next to the page. */
#define TS_PAGE_EXTEND_BYTES ((size_t)4096)

/* Set up an in-use page to hold blocks of `block_size` (a size class size). */
void ts_page_init(ts_page_t *page, size_t block_size);

/* Link the next chunk of never-used blocks into the free list. Requires
 * page->reserved < page->capacity. */
void ts_page_extend(ts_page_t *page);

TS_INLINE uint8_t *ts_page_block_at(const ts_page_t *page, size_t i) {
    ts_segment_t *seg = ts_ptr_segment(page);
    return ts_page_start(seg, page) + i * page->block_size;
}

/* A block from this page, or NULL when every block is in use. */
TS_INLINE void *ts_page_alloc(ts_page_t *page) {
    ts_block_t *b = page->free;
    if (ts_unlikely(b == NULL)) {
        if (page->reserved == page->capacity)
            return NULL;
        ts_page_extend(page);
        b = page->free;
    }
    page->free = b->next;
    page->used++;
    return b;
}

/* Return a block that came from ts_page_alloc on this page. */
TS_INLINE void ts_page_free(ts_page_t *page, void *p) {
    ts_block_t *b = p;
    b->next = page->free;
    page->free = b;
    page->used--;
}

TS_INLINE int ts_page_is_empty(const ts_page_t *page) {
    return page->used == 0;
}

TS_INLINE int ts_page_is_full(const ts_page_t *page) {
    return page->free == NULL && page->reserved == page->capacity;
}

#endif /* TS_PAGE_H */
