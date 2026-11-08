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
    uint8_t in_use;      /* TS_PAGE_FREE, TS_PAGE_USED or TS_PAGE_TAIL */
    uint8_t size_class;
    uint8_t queued;      /* linked into its heap's queue for size_class */
    uint16_t capacity;   /* blocks that fit in the page */
    uint16_t reserved;   /* blocks carved out so far (bump cursor) */
    uint16_t used;       /* blocks currently allocated */
    uint8_t span_pages;  /* 64 KiB pages this page covers (1 unless a span head) */
    uint32_t block_size;
    ts_block_t *free;    /* blocks ready to hand out */
    /* Blocks the owner freed. Kept apart from `free` so malloc runs `free`
     * dry and takes the slow path every so often, which is where remote
     * frees get collected; see ts_page_alloc_slow. */
    ts_block_t *local_free;
    ts_page_t *next;     /* page queue links */
    ts_page_t *prev;
};

/* page->in_use. A span of n pages is one page as far as the heap is
 * concerned: its first descriptor is TS_PAGE_USED and does all the work,
 * the n - 1 after it are TS_PAGE_TAIL and only mark the space as taken.
 * Code that walks a segment's pages looks at TS_PAGE_USED ones only. */
#define TS_PAGE_FREE 0
#define TS_PAGE_USED 1
#define TS_PAGE_TAIL 2

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
    /* Pages that are free but may still hold resident memory, one bit per
     * page. `dirty_seen` is the copy taken at the last purge sweep: a page
     * in both has been free for at least a whole sweep period. Reusing a
     * page clears its bits, which restarts its clock. Owner only. */
    uint64_t dirty;
    uint64_t dirty_seen;
    /* span_offset[i]: how far page i is from the first page of the span
     * that covers it (0 for a span's first page and for ordinary pages).
     * Read by every free to find the descriptor of a block; written only
     * while a span is set up or torn down, when no block of it is out, so
     * it sits with the other read-mostly fields, away from the descriptors. */
    uint8_t span_offset[TS_PAGES_PER_SEGMENT];
    /* Everything above and the page descriptors are written only by the
     * owner (after setup), and the descriptors on every malloc and free. */
    ts_page_t pages[TS_PAGES_PER_SEGMENT];
    /* Blocks freed by other threads into each page, pushed lock-free; the
     * owner splices them into `free` when it runs dry. The low two bits are
     * flags (TS_TF_FULL, TS_TF_BUSY in page.h).
     *
     * These are the only words other threads write, so they live on their
     * own cache lines rather than in the descriptors: a remote free into a
     * page used to bounce the line holding that page's `free` and `used`
     * (and its neighbours') away from the owner in the middle of a malloc.
     * The owner reads them only on its slow path. */
    _Alignas(TS_CACHE_LINE) _Atomic uintptr_t thread_free[TS_PAGES_PER_SEGMENT];
};

/* Page 0's data starts at this offset rather than right after the header.
 * Every page then starts on a 4 KiB boundary, so a block whose size is a
 * multiple of an alignment up to 4 KiB is aligned in any page. That is what
 * lets small aligned_alloc requests come from ordinary size classes. */
#define TS_PAGE0_OFFSET ((size_t)4096)

_Static_assert(sizeof(ts_segment_t) <= TS_PAGE0_OFFSET, "segment header fits before page 0 data");
_Static_assert(TS_PAGES_PER_SEGMENT <= 64, "one bit per page in the dirty masks");
_Static_assert(offsetof(ts_segment_t, thread_free) % TS_CACHE_LINE == 0,
               "remote-free lists don't share a line with the descriptors");
/* The segment header ends in the thread_free lines, and page 0 data starts
 * on its own line after them, so blocks never share one either. */
_Static_assert(TS_PAGE0_OFFSET % TS_CACHE_LINE == 0, "page 0 data starts a cache line");

/* Fresh segment from the OS with every page free, or NULL. */
ts_segment_t *ts_segment_alloc(void);
void ts_segment_free(ts_segment_t *seg);

/* Segments currently mapped, process wide. */
size_t ts_segment_count(void);

/* A free page of `seg`, now marked in use, or NULL if the segment is full. */
ts_page_t *ts_segment_page_alloc(ts_segment_t *seg);

/* `npages` (1 .. TS_PAGES_PER_SEGMENT) contiguous free pages of `seg` as one
 * span, or NULL if no run that long is free. Returns the first descriptor,
 * with span_pages set. */
ts_page_t *ts_segment_span_alloc(ts_segment_t *seg, size_t npages);

/* Give back a page or a whole span (pass its first descriptor). */
void ts_segment_page_free(ts_segment_t *seg, ts_page_t *page);

/* Decommit the free pages that have been dirty since the previous call, or
 * every dirty page if `all` is set. Returns how many pages that was. The
 * header is safe: page 0's range starts past it, and decommit rounds inward
 * to whole OS pages. */
size_t ts_segment_purge(ts_segment_t *seg, int all);

/* Segment containing any address inside it (only meaningful for pointers
 * that came from segments). */
TS_INLINE ts_segment_t *ts_ptr_segment(const void *p) {
    return (ts_segment_t *)((uintptr_t)p & ~(uintptr_t)TS_SEGMENT_MASK);
}

/* Index of the page (or span) holding p: the 64 KiB slot it falls in,
 * moved back to the start of its span. */
TS_INLINE size_t ts_ptr_page_index(const ts_segment_t *seg, const void *p) {
    size_t idx = ((uintptr_t)p - (uintptr_t)seg) >> TS_PAGE_SHIFT;
    return idx - seg->span_offset[idx];
}

TS_INLINE ts_page_t *ts_ptr_page(ts_segment_t *seg, const void *p) {
    return &seg->pages[ts_ptr_page_index(seg, p)];
}

/* A page's remote-free list. Works from the descriptor alone: descriptors
 * live in their segment's header, so masking finds the segment. */
TS_INLINE _Atomic uintptr_t *ts_page_thread_free(ts_page_t *page) {
    return &ts_ptr_segment(page)->thread_free[page->index];
}

/* The same list, found from a block's address with no load at all. Remote
 * freers use this so they never read the owner's descriptor line. */
TS_INLINE _Atomic uintptr_t *ts_ptr_thread_free(ts_segment_t *seg, const void *p) {
    return &seg->thread_free[ts_ptr_page_index(seg, p)];
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
    size_t bytes = (size_t)page->span_pages << TS_PAGE_SHIFT;
    return page->index == 0 ? bytes - TS_PAGE0_OFFSET : bytes;
}

#endif /* TS_SEGMENT_H */
