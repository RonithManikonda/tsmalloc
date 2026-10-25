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
