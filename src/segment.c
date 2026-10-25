#include "segment.h"

#include "os.h"

ts_segment_t *ts_segment_alloc(void) {
    ts_segment_t *seg = ts_os_alloc_aligned(TS_SEGMENT_SIZE, TS_SEGMENT_SIZE);
    if (seg == NULL)
        return NULL;
    /* mmap memory is zeroed, so only the non-zero fields need setting. */
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++)
        seg->pages[i].index = (uint8_t)i;
    return seg;
}

void ts_segment_free(ts_segment_t *seg) {
    ts_os_free(seg, TS_SEGMENT_SIZE);
}

ts_page_t *ts_segment_page_alloc(ts_segment_t *seg) {
    if (seg->used_pages == TS_PAGES_PER_SEGMENT)
        return NULL;
    /* Linear scan over 64 bytes of descriptors; this is the slow path that
     * runs once per 64 KiB of new blocks. Lowest index first keeps a
     * segment's live pages packed toward the front. */
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        ts_page_t *page = &seg->pages[i];
        if (!page->in_use) {
            page->in_use = 1;
            seg->used_pages++;
            return page;
        }
    }
    return NULL; /* unreachable while used_pages is accurate */
}

void ts_segment_page_free(ts_segment_t *seg, ts_page_t *page) {
    page->in_use = 0;
    seg->used_pages--;
}
