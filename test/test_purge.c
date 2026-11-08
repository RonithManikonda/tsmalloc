#include "test.h"

#include "heap.h"
#include "os.h"
#include "tsmalloc.h"

#include <stdlib.h>
#include <string.h>

#define BLOCK 1024
#define MAX_BLOCKS 512

static ts_heap_t heap;

static void heap_setup(long delay) {
    ts_heap_init(&heap);
    ts_heap_set_purge_delay(&heap, delay);
}

static void block_free(void *p) {
    ts_segment_t *seg = ts_ptr_segment(p);
    ts_heap_free_small(&heap, seg, ts_ptr_page(seg, p), p);
}

/* `n` blocks, which at 1 KiB each spread over about n / 64 pages. */
static void alloc_blocks(void **out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] = ts_heap_malloc_small(&heap, BLOCK);
        memset(out[i], 0x3C, BLOCK);
    }
}

static void free_blocks(void **blocks, size_t n) {
    for (size_t i = 0; i < n; i++)
        block_free(blocks[i]);
}

static void ticks(size_t n) {
    for (size_t i = 0; i < n; i++)
        ts_heap_tick(&heap);
}

static uint64_t dirty_mask(void) {
    return heap.segments != NULL ? heap.segments->dirty : 0;
}

TEST(retired_pages_are_marked_dirty) {
    heap_setup(-1);
    void *blocks[256];
    alloc_blocks(blocks, 256);
    CHECK_EQ(dirty_mask(), 0);
    /* Keep the last block so the segment stays linked. */
    free_blocks(blocks, 255);
    CHECK(dirty_mask() != 0);
    block_free(blocks[255]);
    ts_heap_abandon(&heap);
}

TEST(nothing_is_purged_before_a_full_period) {
    heap_setup(100);
    void *blocks[256];
    alloc_blocks(blocks, 256);
    free_blocks(blocks, 255);
    uint64_t before = heap.purged_pages;

    /* The first sweep only notes which pages are dirty, whenever it lands. */
    ticks((size_t)(heap.purge_next - heap.tick) - 1);
    CHECK_EQ(heap.purged_pages, before);
    ticks(1);
    CHECK_EQ(heap.purged_pages, before);
    CHECK(heap.segments->dirty_seen != 0);

    ticks(100);
    CHECK(heap.purged_pages > before);
    CHECK_EQ(heap.segments->dirty & heap.segments->dirty_seen, 0);
    block_free(blocks[255]);
    ts_heap_abandon(&heap);
}

TEST(delay_zero_purges_as_pages_empty) {
    heap_setup(0);
    void *blocks[256];
    alloc_blocks(blocks, 256);
    free_blocks(blocks, 255);
    CHECK(heap.purged_pages > 0);
    CHECK_EQ(dirty_mask(), 0);
    block_free(blocks[255]);
    ts_heap_abandon(&heap);
}

TEST(negative_delay_never_purges) {
    heap_setup(-1);
    void *blocks[256];
    alloc_blocks(blocks, 256);
    free_blocks(blocks, 255);
    ticks(100000);
    CHECK_EQ(heap.purged_pages, 0);
    CHECK(dirty_mask() != 0);
    block_free(blocks[255]);
    ts_heap_abandon(&heap);
}

TEST(reuse_restarts_a_pages_clock) {
    heap_setup(-1);
    void *blocks[256];
    alloc_blocks(blocks, 256);
    free_blocks(blocks, 255);
    uint64_t dirty = dirty_mask();
    REQUIRE(dirty != 0);

    /* One sweep's worth of ageing, by hand. */
    CHECK_EQ(ts_segment_purge(heap.segments, 0), 0);
    CHECK_EQ(heap.segments->dirty_seen, dirty);

    /* Everything the heap can hand back comes from those free pages. */
    void *again[256];
    alloc_blocks(again, 255);
    CHECK(dirty_mask() != dirty);
    uint64_t seen_after = heap.segments->dirty_seen;
    CHECK_EQ(seen_after & ~dirty_mask(), 0);

    free_blocks(again, 255);
    /* Freed again, but not seen by a sweep since: nothing is stale yet
     * beyond what stayed free the whole time. */
    size_t expect = (size_t)__builtin_popcountll(heap.segments->dirty & heap.segments->dirty_seen);
    CHECK_EQ(ts_segment_purge(heap.segments, 0), expect);
    block_free(blocks[255]);
    ts_heap_abandon(&heap);
}

TEST(purging_leaves_live_blocks_alone) {
    heap_setup(0);
    enum { N = MAX_BLOCKS };
    static void *blocks[N];
    alloc_blocks(blocks, N);
    /* Every other block's page stays live only if a block of it survives,
     * so keep one in every 40. */
    for (size_t i = 0; i < N; i++)
        if (i % 40 != 0)
            block_free(blocks[i]);
    for (size_t i = 0; i < N; i += 40) {
        unsigned char *p = blocks[i];
        for (size_t j = sizeof(void *); j < BLOCK; j++)
            CHECK_EQ(p[j], 0x3C);
    }
    for (size_t i = 0; i < N; i += 40)
        block_free(blocks[i]);
    ts_heap_abandon(&heap);
}

TEST(purged_pages_are_usable_again) {
    heap_setup(0);
    void *blocks[256];
    for (int round = 0; round < 5; round++) {
        alloc_blocks(blocks, 256);
        for (size_t i = 0; i < 256; i++) {
            unsigned char *p = blocks[i];
            CHECK_EQ(p[BLOCK - 1], 0x3C);
        }
        free_blocks(blocks, 256);
    }
    CHECK(heap.purged_pages > 0);
    ts_heap_abandon(&heap);
}

TEST(idle_spare_page_is_retired_by_a_sweep) {
    heap_setup(50);
    void *p = ts_heap_malloc_small(&heap, BLOCK);
    REQUIRE(p != NULL);
    unsigned cls = ts_size_to_class(BLOCK);
    block_free(p);
    CHECK(heap.spare[cls] != NULL);

    ticks((size_t)(heap.purge_next - heap.tick));
    CHECK(heap.spare[cls] == NULL);
    CHECK(heap.pages[cls].head == NULL);

    /* The class still works, and the direct slot wasn't left dangling. */
    p = ts_heap_malloc_small(&heap, BLOCK);
    REQUIRE(p != NULL);
    block_free(p);
    ts_heap_abandon(&heap);
}

TEST(cached_segment_is_purged_and_keeps_its_header) {
    heap_setup(0);
    void *p = ts_heap_malloc_small(&heap, TS_SMALL_MAX);
    REQUIRE(p != NULL);
    ts_segment_t *seg = ts_ptr_segment(p);
    block_free(p);
    ticks(2); /* retires the spare; the empty segment moves to the cache */
    CHECK(heap.segments == NULL);
    CHECK(heap.seg_cache == seg);
    CHECK_EQ(seg->dirty, 0);

    CHECK_EQ(seg->kind, TS_KIND_SEGMENT);
    p = ts_heap_malloc_small(&heap, TS_SMALL_MAX);
    CHECK_EQ((uintptr_t)ts_ptr_segment(p), (uintptr_t)seg);
    memset(p, 0x11, TS_SMALL_MAX);
    block_free(p);
    ts_heap_abandon(&heap);
}

TEST(medium_span_purges_all_its_pages) {
    heap_setup(0);
    size_t size = 256 * 1024;
    void *keep = ts_heap_malloc_small(&heap, BLOCK);
    REQUIRE(keep != NULL);
    void *a = ts_heap_malloc_small(&heap, size);
    REQUIRE(a != NULL);
    memset(a, 0x22, size);
    block_free(a);
    ticks(1);
    CHECK(heap.purged_pages >= size >> TS_PAGE_SHIFT);
    block_free(keep);
    ts_heap_abandon(&heap);
}

TEST(parse_purge_delay) {
    CHECK_EQ(ts_parse_purge_delay(NULL), TS_PURGE_DELAY_DEFAULT);
    CHECK_EQ(ts_parse_purge_delay(""), TS_PURGE_DELAY_DEFAULT);
    CHECK_EQ(ts_parse_purge_delay("garbage"), TS_PURGE_DELAY_DEFAULT);
    CHECK_EQ(ts_parse_purge_delay("12x"), TS_PURGE_DELAY_DEFAULT);
    CHECK_EQ(ts_parse_purge_delay("0"), 0);
    CHECK_EQ(ts_parse_purge_delay("250"), 250);
    CHECK_EQ(ts_parse_purge_delay("-1"), -1);
}

TEST(environment_sets_the_default) {
    const char *env = getenv("TSMALLOC_PURGE_DELAY");
    CHECK_EQ(ts_purge_delay(), ts_parse_purge_delay(env));
    ts_heap_t *h = ts_os_alloc(sizeof *h);
    REQUIRE(h != NULL);
    ts_heap_init(h);
    CHECK_EQ(h->purge_delay, ts_parse_purge_delay(env));
    ts_os_free(h, sizeof *h);
}
