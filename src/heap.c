#include "heap.h"

void ts_heap_init(ts_heap_t *heap) {
    *heap = (ts_heap_t){0};
    pthread_mutex_init(&heap->remote_lock, NULL);
}

static void queue_push_front(ts_page_queue_t *q, ts_page_t *page) {
    page->prev = NULL;
    page->next = q->head;
    if (q->head != NULL)
        q->head->prev = page;
    q->head = page;
    page->queued = 1;
}

static void queue_remove(ts_page_queue_t *q, ts_page_t *page) {
    if (page->prev != NULL)
        page->prev->next = page->next;
    else
        q->head = page->next;
    if (page->next != NULL)
        page->next->prev = page->prev;
    page->next = page->prev = NULL;
    page->queued = 0;
}

static void segment_link(ts_heap_t *heap, ts_segment_t *seg) {
    seg->prev = NULL;
    seg->next = heap->segments;
    if (heap->segments != NULL)
        heap->segments->prev = seg;
    heap->segments = seg;
}

static void segment_unlink(ts_heap_t *heap, ts_segment_t *seg) {
    if (seg->prev != NULL)
        seg->prev->next = seg->next;
    else
        heap->segments = seg->next;
    if (seg->next != NULL)
        seg->next->prev = seg->prev;
}

/* A page from any owned segment with room, or from a new segment. */
static ts_page_t *page_fresh(ts_heap_t *heap, unsigned cls) {
    ts_page_t *page = NULL;
    for (ts_segment_t *seg = heap->segments; seg != NULL; seg = seg->next) {
        page = ts_segment_page_alloc(seg);
        if (page != NULL)
            break;
    }
    if (page == NULL) {
        ts_segment_t *seg = ts_segment_alloc();
        if (seg == NULL)
            return NULL;
        seg->heap = heap;
        segment_link(heap, seg);
        page = ts_segment_page_alloc(seg);
    }
    page->size_class = (uint8_t)cls;
    ts_page_init(page, ts_class_size(cls));
    return page;
}

static void page_retire(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page) {
    ts_segment_page_free(seg, page);
    /* Keep the last segment around even when empty: a program that frees
     * everything and starts again shouldn't pay for an mmap each time. */
    if (seg->used_pages == 0 && (seg->next != NULL || seg->prev != NULL)) {
        segment_unlink(heap, seg);
        ts_segment_free(seg);
    }
}

void *ts_heap_malloc_small(ts_heap_t *heap, size_t size) {
    return ts_heap_malloc_class(heap, ts_size_to_class(size));
}

/* A block from the first page in `q` that has one, or NULL. */
TS_INLINE void *queue_alloc(ts_page_queue_t *q) {
    for (ts_page_t *page = q->head; page != NULL; page = q->head) {
        void *p = ts_page_alloc(page);
        if (ts_likely(p != NULL))
            return p;
        /* Full: drop it from the queue until one of its blocks is freed. */
        queue_remove(q, page);
    }
    return NULL;
}

void *ts_heap_malloc_class(ts_heap_t *heap, unsigned cls) {
    ts_page_queue_t *q = &heap->pages[cls];
    void *p = queue_alloc(q);
    if (ts_likely(p != NULL))
        return p;

    /* Take back remote frees before reaching for a fresh page. A thread
     * whose blocks are all freed elsewhere (a producer) would otherwise
     * never see its pages empty and keep growing. */
    if (ts_heap_collect_remote(heap) > 0 && (p = queue_alloc(q)) != NULL)
        return p;

    ts_page_t *page = page_fresh(heap, cls);
    if (page == NULL)
        return NULL;
    queue_push_front(q, page);
    return ts_page_alloc(page);
}

void ts_heap_free_small(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page, void *p) {
    ts_page_free(page, p);
    ts_page_queue_t *q = &heap->pages[page->size_class];

    if (ts_page_is_empty(page)) {
        /* An empty page that is the only one in its queue stays: otherwise
         * a loop of malloc(n); free() would set up and tear down a page on
         * every iteration. */
        if (page->queued && q->head == page && page->next == NULL)
            return;
        if (page->queued)
            queue_remove(q, page);
        page_retire(heap, seg, page);
        return;
    }
    if (!page->queued)
        queue_push_front(q, page);
}

void ts_heap_free_remote(ts_heap_t *heap, void *p) {
    ts_block_t *b = p;
    pthread_mutex_lock(&heap->remote_lock);
    b->next = atomic_load_explicit(&heap->remote_free, memory_order_relaxed);
    atomic_store_explicit(&heap->remote_free, b, memory_order_relaxed);
    pthread_mutex_unlock(&heap->remote_lock);
}

size_t ts_heap_collect_remote(ts_heap_t *heap) {
    /* Unlocked peek, so the common case of nothing to collect costs one
     * load. A push that lands just after it is picked up next time. The
     * blocks' `next` links are only read after taking the lock, which is
     * what orders them after the pushes. */
    if (atomic_load_explicit(&heap->remote_free, memory_order_relaxed) == NULL)
        return 0;
    pthread_mutex_lock(&heap->remote_lock);
    ts_block_t *b = atomic_exchange_explicit(&heap->remote_free, NULL, memory_order_relaxed);
    pthread_mutex_unlock(&heap->remote_lock);

    size_t n = 0;
    while (b != NULL) {
        ts_block_t *next = b->next;
        ts_segment_t *seg = ts_ptr_segment(b);
        ts_heap_free_small(heap, seg, ts_ptr_page(seg, b), b);
        b = next;
        n++;
    }
    return n;
}
