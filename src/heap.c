#include "heap.h"

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>

ts_page_t ts_page_empty;

void ts_heap_init(ts_heap_t *heap) {
    *heap = (ts_heap_t){0};
    for (size_t i = 0; i < TS_DIRECT_SLOTS; i++)
        heap->pages_direct[i] = &ts_page_empty;
    atomic_init(&heap->delayed_free, NULL);
    ts_heap_set_purge_delay(heap, ts_purge_delay());
}

long ts_parse_purge_delay(const char *text) {
    if (text == NULL || *text == '\0')
        return TS_PURGE_DELAY_DEFAULT;
    char *end;
    long value = strtol(text, &end, 10);
    return *end == '\0' ? value : TS_PURGE_DELAY_DEFAULT;
}

long ts_purge_delay(void) {
    /* Racing first callers all compute the same value. getenv and strtol
     * don't allocate, so this is safe on the way into the first malloc. */
    static _Atomic long cached = LONG_MIN;
    long delay = atomic_load_explicit(&cached, memory_order_relaxed);
    if (delay == LONG_MIN) {
        delay = ts_parse_purge_delay(getenv("TSMALLOC_PURGE_DELAY"));
        atomic_store_explicit(&cached, delay, memory_order_relaxed);
    }
    return delay;
}

/* Tick of the sweep after this one. Never 0, which means no sweeps. */
static uint64_t next_sweep(const ts_heap_t *heap) {
    return heap->purge_delay < 0 ? 0 : heap->tick + (uint64_t)(heap->purge_delay > 0 ? heap->purge_delay : 1);
}

void ts_heap_set_purge_delay(ts_heap_t *heap, long delay) {
    heap->purge_delay = delay;
    heap->purge_next = next_sweep(heap);
}

/* Point the direct slots of class `cls` at its queue head. The class covers
 * request sizes (previous class size, its size], i.e. slots prev/16 + 1 up
 * to size/16; class 0 also takes slot 0 for malloc(0). */
static void direct_update(ts_heap_t *heap, unsigned cls) {
    size_t size = ts_class_size(cls);
    if (size > TS_LUT_MAX)
        return;
    size_t lo = cls == 0 ? 0 : ts_class_size(cls - 1) / 16 + 1;
    ts_page_t *head = heap->pages[cls].head;
    if (head == NULL)
        head = &ts_page_empty;
    for (size_t i = lo; i <= size / 16; i++)
        heap->pages_direct[i] = head;
}

static void queue_push_front(ts_heap_t *heap, unsigned cls, ts_page_t *page) {
    ts_page_queue_t *q = &heap->pages[cls];
    page->prev = NULL;
    page->next = q->head;
    if (q->head != NULL)
        q->head->prev = page;
    q->head = page;
    page->queued = 1;
    direct_update(heap, cls);
}

static void queue_remove(ts_heap_t *heap, unsigned cls, ts_page_t *page) {
    ts_page_queue_t *q = &heap->pages[cls];
    if (page->prev != NULL) {
        page->prev->next = page->next;
    } else {
        q->head = page->next;
        direct_update(heap, cls);
    }
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

/* Hold on to an empty segment, or unmap it if the cache is full. Its pages
 * are all free, so no other thread holds a block in it. */
static void segment_cache_push(ts_heap_t *heap, ts_segment_t *seg) {
    if (heap->seg_cache_count == TS_SEGMENT_CACHE_MAX) {
        ts_segment_free(seg);
        return;
    }
    seg->prev = NULL;
    seg->next = heap->seg_cache;
    heap->seg_cache = seg;
    heap->seg_cache_count++;
}

static ts_segment_t *segment_cache_pop(ts_heap_t *heap) {
    ts_segment_t *seg = heap->seg_cache;
    if (seg != NULL) {
        heap->seg_cache = seg->next;
        heap->seg_cache_count--;
    }
    return seg;
}

static void segment_cache_release(ts_heap_t *heap) {
    ts_segment_t *seg;
    while ((seg = segment_cache_pop(heap)) != NULL)
        ts_segment_free(seg);
}

/* A page (or span, for medium classes) from any owned segment with room,
 * else from a cached or new segment. */
static ts_page_t *page_fresh(ts_heap_t *heap, unsigned cls) {
    size_t npages = ts_class_span_pages(cls);
    ts_page_t *page = NULL;
    for (ts_segment_t *seg = heap->segments; seg != NULL; seg = seg->next) {
        page = ts_segment_span_alloc(seg, npages);
        if (page != NULL)
            break;
    }
    if (page == NULL) {
        ts_segment_t *seg = segment_cache_pop(heap);
        if (seg == NULL) {
            seg = ts_segment_alloc();
            if (seg == NULL)
                return NULL;
            seg->heap = heap;
            atomic_store_explicit(&seg->thread_id, heap->thread_id, memory_order_relaxed);
        }
        segment_link(heap, seg);
        page = ts_segment_span_alloc(seg, npages);
    }
    page->size_class = (uint8_t)cls;
    ts_page_init(page, ts_class_size(cls));
    return page;
}

static void page_retire(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page) {
    ts_segment_page_free(seg, page);
    if (seg->used_pages == 0) {
        segment_unlink(heap, seg);
        segment_cache_push(heap, seg);
    }
}

static void heap_purge(ts_heap_t *heap) {
    heap->purge_next = next_sweep(heap);

    /* A spare that sat empty for a whole period isn't saving any churn. Its
     * memory goes back like any other page's. It can't have remote frees
     * pending: those count as used until they are collected. */
    for (unsigned cls = 0; cls < TS_NUM_CLASSES; cls++) {
        ts_page_t *spare = heap->spare[cls];
        if (spare == NULL || !ts_page_is_empty(spare))
            continue;
        heap->spare[cls] = NULL;
        if (spare->queued)
            queue_remove(heap, cls, spare);
        page_retire(heap, ts_ptr_segment(spare), spare);
    }

    int all = heap->purge_delay == 0;
    for (ts_segment_t *seg = heap->segments; seg != NULL; seg = seg->next)
        heap->purged_pages += ts_segment_purge(seg, all);
    for (ts_segment_t *seg = heap->seg_cache; seg != NULL; seg = seg->next)
        heap->purged_pages += ts_segment_purge(seg, all);
}

void ts_heap_tick(ts_heap_t *heap) {
    heap->tick++;
    if (ts_unlikely(heap->purge_next != 0 && heap->tick >= heap->purge_next))
        heap_purge(heap);
}

void *ts_heap_malloc_small(ts_heap_t *heap, size_t size) {
    return ts_heap_malloc_class(heap, ts_size_to_class(size));
}

/* A block from the first page of class `cls` that has one, or NULL. */
TS_INLINE void *queue_alloc(ts_heap_t *heap, unsigned cls) {
    ts_page_queue_t *q = &heap->pages[cls];
    for (ts_page_t *page = q->head; page != NULL; page = q->head) {
        void *p = ts_page_alloc(page);
        if (ts_likely(p != NULL))
            return p;
        /* Full: drop it from the queue until one of its blocks is freed,
         * unless a remote free arrived meanwhile. Then try it again. */
        if (ts_page_mark_full(page))
            queue_remove(heap, cls, page);
    }
    return NULL;
}

void *ts_heap_malloc_class(ts_heap_t *heap, unsigned cls) {
    ts_heap_tick(heap);
    void *p = queue_alloc(heap, cls);
    if (ts_likely(p != NULL))
        return p;

    /* Take back remote frees into full pages before reaching for a fresh
     * one. A thread whose blocks are all freed elsewhere (a producer) would
     * otherwise never see its full pages again and keep growing. */
    if (ts_heap_collect_remote(heap) > 0 && (p = queue_alloc(heap, cls)) != NULL)
        return p;

    /* Then memory a dead thread left behind. Even if the adopted segment
     * has no page of this class, its free pages are used before a new
     * segment is mapped. */
    if (ts_heap_adopt(heap) && (p = queue_alloc(heap, cls)) != NULL)
        return p;

    ts_page_t *page = page_fresh(heap, cls);
    if (page == NULL)
        return NULL;
    queue_push_front(heap, cls, page);
    return ts_page_alloc(page);
}

void ts_heap_free_small(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page, void *p) {
    ts_page_free(page, p);
    ts_heap_page_freed(heap, seg, page);
}

static void page_emptied(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page) {
    unsigned cls = page->size_class;
    /* Keep one empty page per class: otherwise a loop of malloc(n);
     * free() at a page boundary would set up and tear down a page on
     * every iteration. The spare slot may point at a page that has
     * been allocated from since; then this one takes over. */
    ts_page_t *spare = heap->spare[cls];
    if (spare == NULL || spare == page || !ts_page_is_empty(spare)) {
        heap->spare[cls] = page;
        if (!page->queued)
            queue_push_front(heap, cls, page);
        return;
    }
    if (page->queued)
        queue_remove(heap, cls, page);
    page_retire(heap, seg, page);
}

void ts_heap_page_freed(ts_heap_t *heap, ts_segment_t *seg, ts_page_t *page) {
    if (ts_page_is_empty(page)) {
        page_emptied(heap, seg, page);
        /* Last: a sweep may retire this very page. */
        ts_heap_tick(heap);
        return;
    }
    if (!page->queued)
        queue_push_front(heap, page->size_class, page);
}

void ts_heap_free_remote(ts_segment_t *seg, void *p) {
    ts_block_t *b = p;
    _Atomic uintptr_t *tf = ts_ptr_thread_free(seg, p);
    uintptr_t old = atomic_load_explicit(tf, memory_order_relaxed);
    for (;;) {
        if (ts_unlikely(old & TS_TF_BUSY)) {
            /* Someone is handing a block of this page to the heap. That is
             * a handful of instructions; wait rather than queue behind. */
            sched_yield();
            old = atomic_load_explicit(tf, memory_order_relaxed);
            continue;
        }
        if (ts_unlikely(old == TS_TF_FULL)) {
            /* The owner has forgotten this page. Swap the flag for BUSY and
             * send the block through the heap; whoever wins this does that
             * once, and pushes after us go to the page as normal. Acquire
             * pairs with the release in ts_page_mark_full: seg->heap is
             * the heap that flagged the page. */
            if (atomic_compare_exchange_weak_explicit(tf, &old, TS_TF_BUSY,
                                                      memory_order_acquire, memory_order_relaxed))
                break;
            continue;
        }
        b->next = (ts_block_t *)old;
        /* Release: the owner's acquire in ts_page_collect must see b->next.
         * After this succeeds the block, and maybe the whole page, may be
         * reused or unmapped at once, so nothing below touches them. */
        if (atomic_compare_exchange_weak_explicit(tf, &old, (uintptr_t)b,
                                                  memory_order_release, memory_order_relaxed))
            return;
    }

    /* While BUSY is set the owner neither frees this block (it waits in
     * ts_heap_collect_remote) nor abandons the heap, so both stay put. */
    ts_heap_t *heap = seg->heap;
    ts_block_t *head = atomic_load_explicit(&heap->delayed_free, memory_order_relaxed);
    do {
        b->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&heap->delayed_free, &head, b,
                                                    memory_order_release, memory_order_relaxed));
    /* Release: whoever sees BUSY clear also sees the push above. */
    atomic_fetch_and_explicit(tf, ~TS_TF_BUSY, memory_order_release);
}

/* Wait until no remote freer is between flagging `page` BUSY and clearing
 * it again. Acquire, so its push to delayed_free is visible afterwards. */
static uintptr_t wait_not_busy(ts_page_t *page) {
    uintptr_t tf;
    while ((tf = atomic_load_explicit(ts_page_thread_free(page), memory_order_acquire)) & TS_TF_BUSY)
        sched_yield();
    return tf;
}

size_t ts_heap_collect_remote(ts_heap_t *heap) {
    /* Peek first so the common case of nothing to collect is one load. */
    if (atomic_load_explicit(&heap->delayed_free, memory_order_relaxed) == NULL)
        return 0;
    ts_block_t *b = atomic_exchange_explicit(&heap->delayed_free, NULL, memory_order_acquire);

    size_t n = 0;
    while (b != NULL) {
        ts_block_t *next = b->next;
        ts_segment_t *seg = ts_ptr_segment(b);
        ts_page_t *page = ts_ptr_page(seg, b);
        /* The freer may not have cleared BUSY yet. Freeing the block could
         * retire the page, so let it finish first. */
        wait_not_busy(page);
        ts_heap_free_small(heap, seg, page, b);
        b = next;
        n++;
    }
    return n;
}

/* ---- Abandonment -------------------------------------------------------- */

static pthread_mutex_t abandoned_lock = PTHREAD_MUTEX_INITIALIZER;
static ts_segment_t *abandoned; /* linked through next/prev */
/* Written under the lock; read without it as a cheap "anything there?". */
static _Atomic size_t abandoned_count;

/* Clear TS_TF_FULL so remote frees into the page go to its thread_free list
 * from now on, never to the heap. */
static void page_stop_delayed(ts_page_t *page) {
    uintptr_t old = wait_not_busy(page);
    while (old & TS_TF_FULL) {
        /* FULL is only set on an empty list, so this is FULL -> 0. A freer
         * that wins the race to BUSY first makes us wait for it. */
        if (atomic_compare_exchange_weak_explicit(ts_page_thread_free(page), &old, old & ~TS_TF_FULL,
                                                  memory_order_acquire, memory_order_relaxed))
            return;
        if (old & TS_TF_BUSY)
            old = wait_not_busy(page);
    }
}

void ts_heap_abandon(ts_heap_t *heap) {
    /* 1. Close the delayed path, then 2. drain it. After step 1 no freer
     *    can reach the heap, so after 2 nothing is left on it. */
    for (ts_segment_t *seg = heap->segments; seg != NULL; seg = seg->next)
        for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++)
            if (seg->pages[i].in_use == TS_PAGE_USED)
                page_stop_delayed(&seg->pages[i]);
    ts_heap_collect_remote(heap);

    /* 3. Take in remote frees, release empty pages and segments, abandon
     *    the rest. Frees that race with this land on thread_free and wait
     *    there for whoever adopts the segment. */
    ts_segment_t *next;
    for (ts_segment_t *seg = heap->segments; seg != NULL; seg = next) {
        next = seg->next;
        for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
            ts_page_t *page = &seg->pages[i];
            if (page->in_use != TS_PAGE_USED)
                continue;
            ts_page_collect(page);
            page->queued = 0;
            page->next = page->prev = NULL;
            if (ts_page_is_empty(page))
                ts_segment_page_free(seg, page);
        }
        if (seg->used_pages == 0) {
            ts_segment_free(seg);
            continue;
        }
        seg->heap = NULL;
        /* From here on frees by our thread (say, from a later TLS
         * destructor) are remote too, and a new thread that reuses our id
         * doesn't mistake the segment for its own. */
        atomic_store_explicit(&seg->thread_id, 0, memory_order_relaxed);
        pthread_mutex_lock(&abandoned_lock);
        seg->prev = NULL;
        seg->next = abandoned;
        if (abandoned != NULL)
            abandoned->prev = seg;
        abandoned = seg;
        atomic_store_explicit(&abandoned_count,
                              atomic_load_explicit(&abandoned_count, memory_order_relaxed) + 1,
                              memory_order_relaxed);
        pthread_mutex_unlock(&abandoned_lock);
    }
    segment_cache_release(heap);
    ts_heap_init(heap);
}

int ts_heap_adopt(ts_heap_t *heap) {
    /* A heap with owner 0 would leave the segment looking abandoned. */
    if (heap->thread_id == 0)
        return 0;
    if (atomic_load_explicit(&abandoned_count, memory_order_relaxed) == 0)
        return 0;

    /* Popping under the lock is what makes the segment ours; nobody else
     * can reach it from the list, and no thread treats owner 0 as its own,
     * so there is no need to CAS the owner. The lock also orders everything
     * the abandoning thread wrote before us. */
    pthread_mutex_lock(&abandoned_lock);
    ts_segment_t *seg = abandoned;
    if (seg != NULL) {
        abandoned = seg->next;
        if (abandoned != NULL)
            abandoned->prev = NULL;
        atomic_store_explicit(&abandoned_count,
                              atomic_load_explicit(&abandoned_count, memory_order_relaxed) - 1,
                              memory_order_relaxed);
    }
    pthread_mutex_unlock(&abandoned_lock);
    if (seg == NULL)
        return 0;

    /* seg->heap is read by remote freers only after they win a page's FULL
     * flag, and only we set that, after this store (with release). */
    seg->heap = heap;
    atomic_store_explicit(&seg->thread_id, heap->thread_id, memory_order_relaxed);
    segment_link(heap, seg);

    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        ts_page_t *page = &seg->pages[i];
        if (page->in_use != TS_PAGE_USED)
            continue;
        ts_page_collect(page);
        if (ts_page_is_empty(page))
            ts_segment_page_free(seg, page);
        else
            /* Full pages too: queue_alloc drops them on first look and
             * flags them, which is how they rejoin the delayed path. */
            queue_push_front(heap, page->size_class, page);
    }
    /* A segment that came back empty stays: we're about to need a page. */
    return 1;
}

size_t ts_abandoned_count(void) {
    pthread_mutex_lock(&abandoned_lock);
    size_t n = atomic_load_explicit(&abandoned_count, memory_order_relaxed);
    pthread_mutex_unlock(&abandoned_lock);
    return n;
}

void ts_abandoned_visit(void (*fn)(ts_segment_t *seg, void *arg), void *arg) {
    pthread_mutex_lock(&abandoned_lock);
    for (ts_segment_t *seg = abandoned; seg != NULL; seg = seg->next)
        fn(seg, arg);
    pthread_mutex_unlock(&abandoned_lock);
}
