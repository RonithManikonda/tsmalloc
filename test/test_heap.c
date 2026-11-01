#include "test.h"

#include "heap.h"
#include "tsmalloc.h"

/* Tests drive their own heap so they can look at its state; the last test
 * goes through the public API. */

static size_t count_segments(const ts_heap_t *heap) {
    size_t n = 0;
    for (const ts_segment_t *s = heap->segments; s != NULL; s = s->next)
        n++;
    return n;
}

static size_t used_pages(const ts_heap_t *heap) {
    size_t n = 0;
    for (const ts_segment_t *s = heap->segments; s != NULL; s = s->next)
        n += s->used_pages;
    return n;
}

static void free_block(ts_heap_t *heap, void *p) {
    ts_segment_t *seg = ts_ptr_segment(p);
    ts_heap_free_small(heap, seg, ts_ptr_page(seg, p), p);
}

static void release_all(ts_heap_t *heap) {
    while (heap->segments != NULL) {
        ts_segment_t *seg = heap->segments;
        heap->segments = seg->next;
        ts_segment_free(seg);
    }
    while (heap->seg_cache != NULL) {
        ts_segment_t *seg = heap->seg_cache;
        heap->seg_cache = seg->next;
        ts_segment_free(seg);
    }
}

enum { MANY = 20000 };
static size_t *blocks[MANY];

/* Enough blocks of each class to need several pages (and, for the big
 * classes, several segments); tag every word with the block's id. */
TEST(blocks_never_overlap) {
    static const size_t sizes[] = {1, 16, 24, 100, 128, 129, 1000, 1024, 3000, 8192, 16384};
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        ts_heap_t heap = {0};
        size_t size = sizes[si];
        size_t bsize = ts_class_size(ts_size_to_class(size));
        size_t n = size >= 4096 ? 600 : MANY;
        for (size_t i = 0; i < n; i++) {
            blocks[i] = ts_heap_malloc_small(&heap, size);
            REQUIRE(blocks[i] != NULL);
            CHECK_EQ((uintptr_t)blocks[i] % TS_ALIGN, 0);
            for (size_t w = 0; w < bsize / sizeof(size_t); w++)
                blocks[i][w] = i;
        }
        for (size_t i = 0; i < n; i++)
            for (size_t w = 0; w < bsize / sizeof(size_t); w++)
                if (blocks[i][w] != i) {
                    CHECK(!"block overwritten by a neighbour");
                    w = bsize;
                    i = n;
                }
        for (size_t i = 0; i < n; i++)
            free_block(&heap, blocks[i]);
        /* Everything went back: one page kept for the class, one segment kept. */
        CHECK(used_pages(&heap) <= 1);
        CHECK_EQ(count_segments(&heap), 1);
        release_all(&heap);
    }
}

TEST(full_page_rejoins_queue_on_free) {
    ts_heap_t heap = {0};
    unsigned cls = ts_size_to_class(1024);
    size_t per_page = TS_PAGE_SIZE / 1024;
    /* Fill page 0 (shorter, holds the header) and page 1 exactly, then
     * one more block so page 1 is seen full and dropped. */
    size_t n = 0;
    while (used_pages(&heap) < 3) {
        blocks[n] = ts_heap_malloc_small(&heap, 1024);
        REQUIRE(blocks[n] != NULL);
        n++;
    }
    CHECK(n > 2 * per_page - per_page / 8);
    ts_page_t *third = heap.pages[cls].head;
    CHECK(third->next == NULL); /* the two full pages left the queue */

    void *victim = blocks[n - 2]; /* last block of the second page */
    ts_page_t *second = ts_ptr_page(ts_ptr_segment(victim), victim);
    CHECK(!second->queued);
    free_block(&heap, victim);
    CHECK(second->queued);
    CHECK(heap.pages[cls].head == second);
    /* And the next malloc of that class reuses the freed block. */
    CHECK(ts_heap_malloc_small(&heap, 1000) == victim);
    release_all(&heap);
}

TEST(empty_pages_return_to_segment) {
    ts_heap_t heap = {0};
    for (size_t i = 0; i < 4000; i++)
        blocks[i] = ts_heap_malloc_small(&heap, 256);
    size_t peak = used_pages(&heap);
    CHECK(peak >= 4000 * 256 / TS_PAGE_SIZE);
    for (size_t i = 0; i < 4000; i++)
        free_block(&heap, blocks[i]);
    CHECK_EQ(used_pages(&heap), 1);

    /* The pages are reused by another class. */
    for (size_t i = 0; i < 4000; i++)
        blocks[i] = ts_heap_malloc_small(&heap, 64);
    CHECK_EQ(count_segments(&heap), 1);
    for (size_t i = 0; i < 4000; i++)
        free_block(&heap, blocks[i]);
    release_all(&heap);
}

TEST(malloc_free_loop_reuses_block) {
    ts_heap_t heap = {0};
    void *first = ts_heap_malloc_small(&heap, 48);
    free_block(&heap, first);
    for (int i = 0; i < 1000; i++) {
        void *p = ts_heap_malloc_small(&heap, 48);
        CHECK(p == first);
        free_block(&heap, p);
    }
    CHECK_EQ(used_pages(&heap), 1);
    release_all(&heap);
}

/* ---- Retirement --------------------------------------------------------- */

/* One empty page per class stays even when the class has other pages, so a
 * count that hovers at a page boundary doesn't churn pages. */
TEST(one_spare_page_per_class) {
    ts_heap_t heap = {0};
    unsigned cls = ts_size_to_class(1024);
    size_t n = 0;
    while (used_pages(&heap) < 3) {
        blocks[n] = ts_heap_malloc_small(&heap, 1024);
        REQUIRE(blocks[n] != NULL);
        n++;
    }
    /* Empty the first page: it becomes the spare and stays queued. */
    ts_page_t *first = ts_ptr_page(ts_ptr_segment(blocks[0]), blocks[0]);
    size_t i = 0;
    while (i < n && ts_ptr_page(ts_ptr_segment(blocks[i]), blocks[i]) == first)
        free_block(&heap, blocks[i++]);
    CHECK(ts_page_is_empty(first));
    CHECK(first->in_use && first->queued);
    CHECK(heap.spare[cls] == first);
    CHECK_EQ(used_pages(&heap), 3);

    /* Empty the second page too: with a spare already there, it goes. */
    ts_page_t *second = ts_ptr_page(ts_ptr_segment(blocks[i]), blocks[i]);
    while (i < n && ts_ptr_page(ts_ptr_segment(blocks[i]), blocks[i]) == second)
        free_block(&heap, blocks[i++]);
    CHECK(!second->in_use);
    CHECK_EQ(used_pages(&heap), 2);

    /* Allocating from the spare makes it an ordinary page again; the next
     * page to empty takes over the slot. */
    void *p = ts_heap_malloc_small(&heap, 1024);
    CHECK(ts_ptr_page(ts_ptr_segment(p), p) == first);
    while (i < n)
        free_block(&heap, blocks[i++]);
    ts_page_t *third = ts_ptr_page(ts_ptr_segment(blocks[n - 1]), blocks[n - 1]);
    CHECK(heap.spare[cls] == third);
    CHECK(third->in_use);
    free_block(&heap, p);
    CHECK(!first->in_use); /* second empty page of the class */
    CHECK_EQ(used_pages(&heap), 1);
    release_all(&heap);
}

static size_t cached_segments(const ts_heap_t *heap) {
    size_t n = 0;
    for (const ts_segment_t *s = heap->seg_cache; s != NULL; s = s->next)
        n++;
    return n;
}

/* Biggest class, so a segment fills after ~250 blocks. */
enum { BIG = 16384, SEGS = 6 };

static size_t fill_segments(ts_heap_t *heap, size_t segs) {
    size_t n = 0;
    while (count_segments(heap) < segs || used_pages(heap) % TS_PAGES_PER_SEGMENT != 0) {
        blocks[n] = ts_heap_malloc_small(heap, BIG);
        if (blocks[n] == NULL)
            break;
        n++;
    }
    return n;
}

TEST(empty_segments_are_cached_up_to_a_limit) {
    ts_heap_t heap = {0};
    size_t mapped = ts_segment_count();
    size_t n = fill_segments(&heap, SEGS);
    CHECK_EQ(count_segments(&heap), SEGS);
    CHECK_EQ(ts_segment_count(), mapped + SEGS);

    for (size_t i = 0; i < n; i++)
        free_block(&heap, blocks[i]);
    /* The spare page keeps one segment; two more wait in the cache; the
     * rest went back to the OS. */
    CHECK_EQ(count_segments(&heap), 1);
    CHECK_EQ(cached_segments(&heap), TS_SEGMENT_CACHE_MAX);
    CHECK_EQ(ts_segment_count(), mapped + 1 + TS_SEGMENT_CACHE_MAX);

    /* Growing again uses the cached segments before mapping new ones. */
    n = fill_segments(&heap, 1 + TS_SEGMENT_CACHE_MAX);
    CHECK_EQ(cached_segments(&heap), 0);
    CHECK_EQ(ts_segment_count(), mapped + 1 + TS_SEGMENT_CACHE_MAX);
    for (size_t i = 0; i < n; i++)
        free_block(&heap, blocks[i]);
    release_all(&heap);
    CHECK_EQ(ts_segment_count(), mapped);
}

/* Live memory going back and forth across a segment boundary must not
 * map and unmap a segment each time. */
TEST(no_segment_churn_at_a_boundary) {
    ts_heap_t heap = {0};
    size_t n = fill_segments(&heap, 1);
    size_t mapped = ts_segment_count();
    for (int i = 0; i < 1000; i++) {
        void *p = ts_heap_malloc_small(&heap, BIG); /* needs a second segment */
        REQUIRE(p != NULL);
        free_block(&heap, p);
    }
    CHECK(ts_segment_count() <= mapped + 1);
    for (size_t i = 0; i < n; i++)
        free_block(&heap, blocks[i]);
    release_all(&heap);
}

/* Every direct slot names the head of its class's queue, or the empty page. */
static int direct_in_sync(const ts_heap_t *heap) {
    for (size_t s = 0; s < TS_DIRECT_SLOTS; s++) {
        const ts_page_t *head = heap->pages[ts_class_lut[s]].head;
        if (heap->pages_direct[s] != (head != NULL ? head : &ts_page_empty))
            return 0;
    }
    return 1;
}

/* Random mallocs and frees of every size the direct slots cover, crossing
 * page and segment boundaries; after each step the slots must agree with
 * the queues, or the malloc fast path would hand out blocks of the wrong
 * size (or of a page that left its queue). */
TEST(direct_slots_follow_queue_heads) {
    enum { LIVE = 4096, STEPS = 60000 };
    static void *live[LIVE];
    ts_heap_t heap;
    ts_heap_init(&heap);
    CHECK(direct_in_sync(&heap));
    CHECK(heap.pages_direct[0] == &ts_page_empty);

    uint64_t x = 0x9e3779b97f4a7c15u;
    size_t n = 0, bad = 0;
    for (int step = 0; step < STEPS; step++) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        if (n < LIVE && (n == 0 || (x & 3) != 0)) {
            size_t size = (size_t)(x >> 20) % (TS_LUT_MAX + 1);
            void *p = ts_heap_malloc_small(&heap, size);
            REQUIRE(p != NULL);
            live[n++] = p;
        } else {
            size_t i = (size_t)(x >> 24) % n;
            free_block(&heap, live[i]);
            live[i] = live[--n];
        }
        bad += !direct_in_sync(&heap);
    }
    CHECK_EQ(bad, 0);

    /* With a page queued, the slot hands out that page's next block, which
     * is exactly what the fast path in ts_malloc does. */
    for (size_t size = 1; size <= TS_LUT_MAX; size += 37) {
        ts_page_t *page = heap.pages_direct[(size + 15) >> 4];
        if (page->free == NULL)
            continue;
        CHECK(page->block_size >= size);
        CHECK_EQ(page->size_class, ts_size_to_class(size));
    }

    while (n > 0)
        free_block(&heap, live[--n]);
    CHECK(direct_in_sync(&heap));
    release_all(&heap);
}

/* Through ts_malloc, which takes the fast path once a page is queued. */
TEST(public_fast_path_sizes) {
    for (size_t size = 0; size <= TS_LUT_MAX; size++) {
        void *a = ts_malloc(size);
        void *b = ts_malloc(size); /* this one should come off pages_direct */
        REQUIRE(a != NULL && b != NULL);
        CHECK_EQ(ts_usable_size(b), ts_class_size(ts_size_to_class(size)));
        CHECK_EQ((uintptr_t)b % TS_ALIGN, 0);
        if (size > 0)
            memset(b, 0xab, size);
        ts_free(a);
        ts_free(b);
    }
}

/* ts_free finishes most frees inline and hands the page to the heap only
 * when it has to: a full page coming back to its queue, or a page going
 * empty. Fill pages until some leave the queue, then free everything
 * through the public API and check the heap's bookkeeping. */
TEST(public_free_rejoins_and_empties_pages) {
    enum { N = 3 * (TS_PAGE_SIZE / 1024) + 5 };
    static void *ps[N];
    for (size_t i = 0; i < N; i++) {
        ps[i] = ts_malloc(1024);
        REQUIRE(ps[i] != NULL);
    }
    ts_segment_t *seg = ts_ptr_segment(ps[0]);
    ts_heap_t *heap = seg->heap;
    REQUIRE(heap != NULL);
    ts_page_t *first = ts_ptr_page(seg, ps[0]);
    CHECK(!first->queued); /* filled up and dropped by malloc */

    /* One free into a full page puts it back at the front of its queue,
     * where the next malloc of that size finds it straight away. */
    ts_free(ps[0]);
    CHECK(first->queued);
    CHECK(heap->pages_direct[1024 / 16] == first);
    void *again = ts_malloc(1024);
    CHECK(again == ps[0]);
    ps[0] = again;

    size_t live_before = 0;
    for (ts_segment_t *s = heap->segments; s != NULL; s = s->next)
        live_before += s->used_pages;
    for (size_t i = 0; i < N; i++)
        ts_free(ps[i]);
    size_t live_after = 0;
    for (ts_segment_t *s = heap->segments; s != NULL; s = s->next)
        live_after += s->used_pages;
    /* All but the spare page of the class went back to their segment. */
    CHECK(live_after + 3 <= live_before);
    CHECK(direct_in_sync(heap));
}

TEST(public_small_blocks_have_no_header) {
    unsigned char *a = ts_malloc(32);
    unsigned char *b = ts_malloc(32);
    REQUIRE(a != NULL && b != NULL);
    /* Neighbouring blocks of one class, nothing in between. */
    CHECK(b == a + 32 || a == b + 32);
    CHECK_EQ(ts_usable_size(a), 32);
    CHECK_EQ(ts_usable_size(ts_malloc(17)) % 16, 0);
    CHECK(*(uint32_t *)ts_ptr_segment(a) == TS_KIND_SEGMENT);
    ts_free(a);
    ts_free(b);
}
