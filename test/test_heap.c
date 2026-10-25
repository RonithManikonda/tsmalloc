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
