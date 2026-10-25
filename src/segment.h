#ifndef TS_SEGMENT_H
#define TS_SEGMENT_H

#include "internal.h"

/*
 * A segment is a 4 MiB region aligned to 4 MiB. Its first bytes hold this
 * header, including one descriptor per 64 KiB page; page 0's usable area
 * starts right after the header, so it is a little smaller than the others.
 *
 *   segment
 *   +---------+-------------+-------------+-----+--------------+
 *   | header  | page 0 data |   page 1    | ... |   page 63    |
 *   +---------+-------------+-------------+-----+--------------+
 *   ^ p & ~TS_SEGMENT_MASK    ^ (p - segment) >> TS_PAGE_SHIFT == 1
 */

/* A free block's first word links it into its page's free list. Live blocks
 * belong to the user entirely; no per-block header. */
struct ts_block_s {
    ts_block_t *next;
};

/* Page descriptor. Lives in the segment header, not in the page itself, so
 * the page's memory is all blocks (and `ts_ptr_segment(page)` works on the
 * descriptor too). */
struct ts_page_s {
    uint8_t index;       /* position in the segment */
    uint8_t in_use;      /* handed out by ts_segment_page_alloc */
    uint16_t capacity;   /* blocks that fit in the page */
    uint16_t reserved;   /* blocks carved out so far (bump cursor) */
    uint16_t used;       /* blocks currently allocated */
    uint32_t block_size;
    ts_block_t *free;    /* blocks ready to hand out */
};

struct ts_segment_s {
    ts_segment_t *next; /* links for whoever owns the segment */
    ts_segment_t *prev;
    uint32_t used_pages;
    ts_page_t pages[TS_PAGES_PER_SEGMENT];
};

/* Keep the header small next to page 0, or page 0 loses most of its blocks. */
_Static_assert(sizeof(ts_segment_t) <= TS_PAGE_SIZE / 8, "segment header fits early in page 0");

/* Fresh segment from the OS with every page free, or NULL. */
ts_segment_t *ts_segment_alloc(void);
void ts_segment_free(ts_segment_t *seg);

/* A free page of `seg`, now marked in use, or NULL if the segment is full. */
ts_page_t *ts_segment_page_alloc(ts_segment_t *seg);
void ts_segment_page_free(ts_segment_t *seg, ts_page_t *page);

/* Segment containing any address inside it (only meaningful for pointers
 * that came from segments). */
TS_INLINE ts_segment_t *ts_ptr_segment(const void *p) {
    return (ts_segment_t *)((uintptr_t)p & ~(uintptr_t)TS_SEGMENT_MASK);
}

TS_INLINE ts_page_t *ts_ptr_page(ts_segment_t *seg, const void *p) {
    size_t idx = ((uintptr_t)p - (uintptr_t)seg) >> TS_PAGE_SHIFT;
    return &seg->pages[idx];
}

/* Where page data starts: just after the header for page 0, at the 64 KiB
 * boundary for the rest. */
TS_INLINE uint8_t *ts_page_start(ts_segment_t *seg, const ts_page_t *page) {
    uint8_t *start = (uint8_t *)seg + ((size_t)page->index << TS_PAGE_SHIFT);
    if (page->index == 0)
        start += ts_align_up(sizeof(ts_segment_t), TS_ALIGN);
    return start;
}

TS_INLINE size_t ts_page_usable(const ts_page_t *page) {
    return page->index == 0 ? TS_PAGE_SIZE - ts_align_up(sizeof(ts_segment_t), TS_ALIGN)
                            : TS_PAGE_SIZE;
}

#endif /* TS_SEGMENT_H */
