#include "test.h"

#include "segment.h"

TEST(segment_is_aligned_and_empty) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    CHECK_EQ((uintptr_t)seg & TS_SEGMENT_MASK, 0);
    CHECK_EQ(seg->used_pages, 0);
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        CHECK_EQ(seg->pages[i].index, i);
        CHECK_EQ(seg->pages[i].in_use, 0);
    }
    ts_segment_free(seg);
}

TEST(ptr_segment_masks_any_interior_address) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    uint8_t *base = (uint8_t *)seg;
    static const size_t offsets[] = {0, 1, 4095, 65536, 1000003, TS_SEGMENT_SIZE - 1};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++)
        CHECK(ts_ptr_segment(base + offsets[i]) == seg);
    ts_segment_free(seg);
}

TEST(ptr_page_finds_descriptor) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        ts_page_t *page = &seg->pages[i];
        uint8_t *start = ts_page_start(seg, page);
        uint8_t *last = start + ts_page_usable(page) - 1;
        CHECK(ts_ptr_page(seg, start) == page);
        CHECK(ts_ptr_page(seg, last) == page);
        CHECK(ts_ptr_segment(last) == seg);
    }
    ts_segment_free(seg);
}

TEST(page_zero_starts_after_header) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    uint8_t *p0 = ts_page_start(seg, &seg->pages[0]);
    CHECK(p0 >= (uint8_t *)(seg + 1));
    CHECK_EQ((uintptr_t)p0 & (TS_ALIGN - 1), 0);
    CHECK_EQ(p0 + ts_page_usable(&seg->pages[0]), (uint8_t *)seg + TS_PAGE_SIZE);
    CHECK(ts_page_start(seg, &seg->pages[1]) == (uint8_t *)seg + TS_PAGE_SIZE);
    ts_segment_free(seg);
}

TEST(page_alloc_hands_out_every_page_once) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    ts_page_t *pages[TS_PAGES_PER_SEGMENT];
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        pages[i] = ts_segment_page_alloc(seg);
        REQUIRE(pages[i] != NULL);
        CHECK_EQ(pages[i]->index, i);
        /* Whole data area is writable and doesn't clobber the header. */
        memset(ts_page_start(seg, pages[i]), 0xA5, ts_page_usable(pages[i]));
    }
    CHECK_EQ(seg->used_pages, TS_PAGES_PER_SEGMENT);
    CHECK(ts_segment_page_alloc(seg) == NULL);
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++)
        CHECK_EQ(seg->pages[i].in_use, 1);

    ts_segment_page_free(seg, pages[17]);
    CHECK_EQ(seg->used_pages, TS_PAGES_PER_SEGMENT - 1);
    CHECK(ts_segment_page_alloc(seg) == pages[17]);
    ts_segment_free(seg);
}

TEST(segments_are_distinct) {
    enum { N = 16 };
    ts_segment_t *segs[N];
    for (int i = 0; i < N; i++) {
        segs[i] = ts_segment_alloc();
        REQUIRE(segs[i] != NULL);
        for (int j = 0; j < i; j++)
            CHECK(segs[i] != segs[j]);
    }
    for (int i = 0; i < N; i++)
        ts_segment_free(segs[i]);
}

/* Remote freers find a page's thread_free slot from the block address,
 * the owner from the descriptor; both must land on the same word, and
 * none of those words may share a cache line with a descriptor. */
TEST(thread_free_slots_off_the_descriptor_lines) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    uintptr_t desc_end = (uintptr_t)&seg->pages[TS_PAGES_PER_SEGMENT];
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        ts_page_t *page = &seg->pages[i];
        uint8_t *start = ts_page_start(seg, page);
        uint8_t *last = start + ts_page_usable(page) - 1;
        CHECK(ts_ptr_thread_free(seg, start) == ts_page_thread_free(page));
        CHECK(ts_ptr_thread_free(seg, last) == ts_page_thread_free(page));
        uintptr_t tf = (uintptr_t)ts_page_thread_free(page);
        CHECK(tf / TS_CACHE_LINE > (desc_end - 1) / TS_CACHE_LINE);
        CHECK(tf < (uintptr_t)ts_page_start(seg, &seg->pages[0]));
    }
    ts_segment_free(seg);
}

/* ---- Spans --------------------------------------------------------------- */

TEST(span_covers_contiguous_pages) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    ts_page_t *a = ts_segment_span_alloc(seg, 5);
    REQUIRE(a != NULL);
    CHECK_EQ(a->index, 0);
    CHECK_EQ(a->span_pages, 5);
    CHECK_EQ(a->in_use, TS_PAGE_USED);
    CHECK_EQ(seg->used_pages, 5);
    for (size_t j = 1; j < 5; j++) {
        CHECK_EQ(seg->pages[j].in_use, TS_PAGE_TAIL);
        CHECK_EQ(seg->span_offset[j], j);
    }
    /* Page 0 loses the header's share; the rest is whole pages. */
    CHECK_EQ(ts_page_usable(a), 5 * TS_PAGE_SIZE - TS_PAGE0_OFFSET);

    ts_page_t *b = ts_segment_span_alloc(seg, 3);
    REQUIRE(b != NULL);
    CHECK_EQ(b->index, 5);
    CHECK_EQ(ts_page_usable(b), 3 * TS_PAGE_SIZE);

    /* Every address in a span leads to its first descriptor and its
     * thread_free slot, which is what free and remote free rely on. */
    uint8_t *start = ts_page_start(seg, b);
    for (size_t off = 0; off < ts_page_usable(b); off += 4093) {
        CHECK(ts_ptr_page(seg, start + off) == b);
        CHECK(ts_ptr_thread_free(seg, start + off) == ts_page_thread_free(b));
    }
    CHECK(ts_ptr_page(seg, start + ts_page_usable(b) - 1) == b);
    CHECK(ts_ptr_page(seg, start + ts_page_usable(b)) == &seg->pages[8]);
    CHECK(ts_ptr_page(seg, ts_page_start(seg, a) + 4 * TS_PAGE_SIZE) == a);

    /* Freeing gives back every page of the span. */
    ts_segment_page_free(seg, a);
    CHECK_EQ(seg->used_pages, 3);
    for (size_t j = 0; j < 5; j++) {
        CHECK_EQ(seg->pages[j].in_use, TS_PAGE_FREE);
        CHECK_EQ(seg->span_offset[j], 0);
        CHECK_EQ(seg->pages[j].span_pages, 1);
    }
    ts_segment_free(seg);
}

/* First fit skips runs that are too short and reports a full segment. */
TEST(span_alloc_finds_a_long_enough_run) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    ts_page_t *p[4];
    for (int i = 0; i < 4; i++)
        p[i] = ts_segment_page_alloc(seg);
    ts_segment_page_free(seg, p[1]); /* a one-page hole at index 1 */
    ts_page_t *s = ts_segment_span_alloc(seg, 2);
    REQUIRE(s != NULL);
    CHECK_EQ(s->index, 4);
    CHECK(ts_segment_page_alloc(seg) == p[1]); /* single pages fill holes */

    /* 58 pages left from 6 on: a 59-page span can't fit, 58 can. */
    CHECK(ts_segment_span_alloc(seg, 59) == NULL);
    ts_page_t *rest = ts_segment_span_alloc(seg, 58);
    REQUIRE(rest != NULL);
    CHECK_EQ(rest->index, 6);
    CHECK_EQ(seg->used_pages, TS_PAGES_PER_SEGMENT);
    CHECK(ts_segment_page_alloc(seg) == NULL);

    /* A span over the whole segment once it is empty again. */
    ts_segment_page_free(seg, rest);
    ts_segment_page_free(seg, s);
    for (int i = 0; i < 4; i++)
        ts_segment_page_free(seg, p[i]);
    CHECK_EQ(seg->used_pages, 0);
    ts_page_t *all = ts_segment_span_alloc(seg, TS_PAGES_PER_SEGMENT);
    REQUIRE(all != NULL);
    CHECK_EQ(all->index, 0);
    CHECK(ts_ptr_page(seg, (uint8_t *)seg + TS_SEGMENT_SIZE - 1) == all);
    ts_segment_page_free(seg, all);
    ts_segment_free(seg);
}
