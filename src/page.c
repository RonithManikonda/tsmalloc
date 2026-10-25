#include "page.h"

void ts_page_init(ts_page_t *page, size_t block_size) {
    page->block_size = (uint32_t)block_size;
    page->capacity = (uint16_t)(ts_page_usable(page) / block_size);
    page->reserved = 0;
    page->used = 0;
    page->free = NULL;
}

void ts_page_extend(ts_page_t *page) {
    size_t count = TS_PAGE_EXTEND_BYTES / page->block_size;
    if (count == 0)
        count = 1; /* blocks bigger than one chunk go one at a time */
    size_t left = (size_t)(page->capacity - page->reserved);
    if (count > left)
        count = left;

    /* Link in address order so consecutive mallocs return neighbouring
     * blocks, which is friendlier to the cache and the prefetcher. */
    uint8_t *first = ts_page_block_at(page, page->reserved);
    uint8_t *p = first;
    for (size_t i = 1; i < count; i++) {
        uint8_t *next = p + page->block_size;
        ((ts_block_t *)p)->next = (ts_block_t *)next;
        p = next;
    }
    ((ts_block_t *)p)->next = page->free;
    page->free = (ts_block_t *)first;
    page->reserved += (uint16_t)count;
}
