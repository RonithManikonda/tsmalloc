#ifndef TS_SEGMENT_H
#define TS_SEGMENT_H

#include "internal.h"

#include <stdatomic.h>

/*
 * A segment is a 4 MiB region aligned to 4 MiB. Its first bytes hold this
 * header, including one descriptor per 64 KiB page; page 0's usable area
 * starts right after the header, so it is a little smaller than the others.
 *
 *   segment
 *   +---------+-------------+-------------+-----+--------------+
 *   | header  | page 0 data |   page 1    | ... |   page 63    |   (page 0 data at +4 KiB)
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
    uint8_t size_class;
    uint8_t queued;      /* linked into its heap's queue for size_class */
    uint16_t capacity;   /* blocks that fit in the page */
    uint16_t reserved;   /* blocks carved out so far (bump cursor) */
    uint16_t used;       /* blocks currently allocated */
    uint32_t block_size;
    ts_block_t *free;    /* blocks ready to hand out */
    /* Blocks freed by other threads, pushed lock-free; the owner splices
     * them into `free` when it runs dry. The low two bits are flags
     * (TS_TF_FULL, TS_TF_BUSY in page.h). */
    _Atomic uintptr_t thread_free;
    ts_page_t *next;     /* page queue links */
    ts_page_t *prev;
};

/* First word of every region a pointer can come from, so free can tell a
 * page-backed block from a large mapping after one mask. */
#define TS_KIND_SEGMENT 0x5e67a11cu
#define TS_KIND_LARGE   0x4c47ae11u

struct ts_segment_s {
    uint32_t kind;      /* TS_KIND_SEGMENT */
    /* ts_thread_id() of the thread whose heap owns the segment, or 0 when
     * no live thread does. free compares it with its own id: equal means
     * the block is local and goes straight back on its page. Only the
     * owner ever stores its own id here, so a thread can't see its id by
     * mistake; other threads just see "not mine". */
    _Atomic uintptr_t thread_id;
    /* Heap that allocates from this segment. Set once when the heap creates
     * the segment and read by any thread that frees one of its blocks; the
     * block's own handoff between threads orders that read after the write. */
    ts_heap_t *heap;
    ts_segment_t *next; /* links for whoever owns the segment */
    ts_segment_t *prev;
    uint32_t used_pages;
    ts_page_t pages[TS_PAGES_PER_SEGMENT];
};

/* Page 0's data starts at this offset rather than right after the header.
 * Every page then starts on a 4 KiB boundary, so a block whose size is a
 * multiple of an alignment up to 4 KiB is aligned in any page. That is what
 * lets small aligned_alloc requests come from ordinary size classes. */
#define TS_PAGE0_OFFSET ((size_t)4096)

_Static_assert(sizeof(ts_segment_t) <= TS_PAGE0_OFFSET, "segment header fits before page 0 data");

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

/* Where page data starts: TS_PAGE0_OFFSET in for page 0 (after the header),
 * at the 64 KiB boundary for the rest. */
TS_INLINE uint8_t *ts_page_start(ts_segment_t *seg, const ts_page_t *page) {
    uint8_t *start = (uint8_t *)seg + ((size_t)page->index << TS_PAGE_SHIFT);
    if (page->index == 0)
        start += TS_PAGE0_OFFSET;
    return start;
}

TS_INLINE size_t ts_page_usable(const ts_page_t *page) {
    return page->index == 0 ? TS_PAGE_SIZE - TS_PAGE0_OFFSET : TS_PAGE_SIZE;
}

#endif /* TS_SEGMENT_H */
