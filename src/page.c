#include "page.h"

void ts_page_init(ts_page_t *page, size_t block_size) {
    page->block_size = (uint32_t)block_size;
    page->capacity = (uint16_t)(ts_page_usable(page) / block_size);
    page->reserved = 0;
    page->used = 0;
    page->free = NULL;
    /* No other thread can hold a block of a page being set up, so nothing
     * races with this store. */
    atomic_store_explicit(&page->thread_free, 0, memory_order_relaxed);
}

size_t ts_page_collect(ts_page_t *page) {
    if ((atomic_load_explicit(&page->thread_free, memory_order_relaxed) & ~TS_TF_FLAGS) == 0)
        return 0;
    /* Take the whole list, keep the flags. Acquire pairs with the release
     * CAS in ts_heap_free_remote, so the pushers' `next` links are visible. */
    uintptr_t tf = atomic_fetch_and_explicit(&page->thread_free, TS_TF_FLAGS, memory_order_acquire);
    ts_block_t *head = (ts_block_t *)(tf & ~TS_TF_FLAGS);

    /* Walking the list costs one load per block, but `used` needs the count
     * anyway and the blocks are about to be reused. */
    size_t n = 1;
    ts_block_t *tail = head;
    while (tail->next != NULL) {
        tail = tail->next;
        n++;
    }
    tail->next = page->free;
    page->free = head;
    page->used -= (uint16_t)n;
    return n;
}

void *ts_page_alloc_slow(ts_page_t *page) {
    /* Reuse blocks that came back before carving new ones: they are warm
     * in some cache, and an untouched block stays untouched. */
    if (ts_page_collect(page) == 0) {
        if (page->reserved == page->capacity)
            return NULL;
        ts_page_extend(page);
    }
    ts_block_t *b = page->free;
    page->free = b->next;
    page->used++;
    return b;
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
