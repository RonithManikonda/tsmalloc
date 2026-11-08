#include "segment.h"

#include "os.h"

/* Only touched on the slow paths that map and unmap segments. */
static _Atomic size_t live_segments;

ts_segment_t *ts_segment_alloc(void) {
    ts_segment_t *seg = ts_os_alloc_aligned(TS_SEGMENT_SIZE, TS_SEGMENT_SIZE);
    if (seg == NULL)
        return NULL;
    /* mmap memory is zeroed, so only the non-zero fields need setting. */
    atomic_fetch_add_explicit(&live_segments, 1, memory_order_relaxed);
    seg->kind = TS_KIND_SEGMENT;
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        seg->pages[i].index = (uint8_t)i;
        seg->pages[i].span_pages = 1; /* a free page is a span of one */
    }
    return seg;
}

void ts_segment_free(ts_segment_t *seg) {
    ts_os_free(seg, TS_SEGMENT_SIZE);
    atomic_fetch_sub_explicit(&live_segments, 1, memory_order_relaxed);
}

size_t ts_segment_count(void) {
    return atomic_load_explicit(&live_segments, memory_order_relaxed);
}

/* Bits `first` .. `first + n - 1`. */
static uint64_t page_mask(size_t first, size_t n) {
    return (n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1)) << first;
}

ts_page_t *ts_segment_page_alloc(ts_segment_t *seg) {
    return ts_segment_span_alloc(seg, 1);
}

ts_page_t *ts_segment_span_alloc(ts_segment_t *seg, size_t npages) {
    if (TS_PAGES_PER_SEGMENT - seg->used_pages < npages)
        return NULL;
    /* First fit over 64 descriptors, on the slow path that runs once per
     * page or span of new blocks. Lowest index first keeps a segment's
     * live pages packed toward the front, which leaves long free runs at
     * the back for spans. */
    size_t run = 0;
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        if (seg->pages[i].in_use != TS_PAGE_FREE) {
            run = 0;
            continue;
        }
        if (++run < npages)
            continue;
        size_t first = i + 1 - npages;
        ts_page_t *page = &seg->pages[first];
        page->in_use = TS_PAGE_USED;
        page->span_pages = (uint8_t)npages;
        for (size_t j = 1; j < npages; j++) {
            /* Tails keep span_pages == 1; only the head's count matters. */
            seg->pages[first + j].in_use = TS_PAGE_TAIL;
            seg->span_offset[first + j] = (uint8_t)j;
        }
        seg->used_pages += (uint32_t)npages;
        /* Nothing to recommit even if these were purged: MADV_FREE'd memory
         * is simply written to again. */
        uint64_t mask = page_mask(first, npages);
        seg->dirty &= ~mask;
        seg->dirty_seen &= ~mask;
        return page;
    }
    return NULL;
}

void ts_segment_page_free(ts_segment_t *seg, ts_page_t *page) {
    size_t n = page->span_pages;
    seg->dirty |= page_mask(page->index, n);
    for (size_t j = 1; j < n; j++) {
        seg->pages[page->index + j].in_use = TS_PAGE_FREE;
        seg->span_offset[page->index + j] = 0;
    }
    page->in_use = TS_PAGE_FREE;
    page->span_pages = 1;
    seg->used_pages -= (uint32_t)n;
}

size_t ts_segment_purge(ts_segment_t *seg, int all) {
    uint64_t stale = all ? seg->dirty : seg->dirty & seg->dirty_seen;
    seg->dirty &= ~stale;
    seg->dirty_seen = seg->dirty;

    size_t purged = (size_t)__builtin_popcountll(stale);
    /* One madvise per run of adjacent pages, not one per page. */
    while (stale != 0) {
        size_t first = (size_t)__builtin_ctzll(stale);
        uint64_t gap = ~(stale >> first);
        size_t run = gap != 0 ? (size_t)__builtin_ctzll(gap) : 64 - first;
        stale &= ~page_mask(first, run);

        size_t skip = first == 0 ? TS_PAGE0_OFFSET : 0;
        ts_os_decommit((char *)seg + (first << TS_PAGE_SHIFT) + skip, (run << TS_PAGE_SHIFT) - skip);
    }
    return purged;
}
