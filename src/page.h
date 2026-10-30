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

/* Flags in the low bits of page->thread_free (blocks are 16-aligned).
 *
 * TS_TF_FULL: the page was full and the owner took it off its queue, so it
 * won't look at thread_free again. The first remote free to see the flag
 * swaps it for TS_TF_BUSY and hands its block to the heap instead, which
 * gets the page re-queued.
 *
 * TS_TF_BUSY: a remote freer is pushing onto the heap's delayed list. While
 * it is set other freers wait, and the owner doesn't free delayed blocks of
 * this page or give up the heap, so the freer may still touch both.
 *
 * Either flag is only ever set while the list is empty, and never both. */
#define TS_TF_FULL  ((uintptr_t)1)
#define TS_TF_BUSY  ((uintptr_t)2)
#define TS_TF_FLAGS (TS_TF_FULL | TS_TF_BUSY)

/* Set up an in-use page to hold blocks of `block_size` (a size class size). */
void ts_page_init(ts_page_t *page, size_t block_size);

/* Link the next chunk of never-used blocks into the free list. Requires
 * page->reserved < page->capacity. */
void ts_page_extend(ts_page_t *page);

TS_INLINE uint8_t *ts_page_block_at(const ts_page_t *page, size_t i) {
    ts_segment_t *seg = ts_ptr_segment(page);
    return ts_page_start(seg, page) + i * page->block_size;
}

/* Owner only: move blocks other threads freed into `free`. Returns how many. */
size_t ts_page_collect(ts_page_t *page);

/* ts_page_alloc once `free` is empty: remote frees first, then new blocks. */
void *ts_page_alloc_slow(ts_page_t *page);

/* A block from this page, or NULL when every block is in use. */
TS_INLINE void *ts_page_alloc(ts_page_t *page) {
    ts_block_t *b = page->free;
    if (ts_unlikely(b == NULL))
        return ts_page_alloc_slow(page);
    page->free = b->next;
    page->used++;
    return b;
}

/* Owner only, after ts_page_alloc returned NULL: flag the page full before
 * taking it off its queue. Fails (returns 0) if a remote free slipped in
 * since, in which case the page has blocks again, or one is still busy
 * handing a block to the heap; either way the caller just tries the page
 * again. Release so a freer that sees the flag also sees seg->heap. */
TS_INLINE int ts_page_mark_full(ts_page_t *page) {
    uintptr_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(&page->thread_free, &expected, TS_TF_FULL,
                                                memory_order_release, memory_order_relaxed))
        return 1;
    return expected == TS_TF_FULL; /* still flagged from the last time */
}

/* Return a block that came from ts_page_alloc on this page. Owner only. */
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
