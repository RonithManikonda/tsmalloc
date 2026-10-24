#include "test.h"

#include "internal.h"

TEST(layout_constants) {
    CHECK_EQ(TS_SEGMENT_SIZE, 4 << 20);
    CHECK_EQ(TS_PAGE_SIZE, 64 << 10);
    CHECK_EQ(TS_PAGES_PER_SEGMENT, 64);
    CHECK_EQ(TS_SMALL_MAX, 16 << 10);
}

TEST(align_up_down) {
    CHECK_EQ(ts_align_up(0, 16), 0);
    CHECK_EQ(ts_align_up(1, 16), 16);
    CHECK_EQ(ts_align_up(16, 16), 16);
    CHECK_EQ(ts_align_up(17, 16), 32);
    CHECK_EQ(ts_align_up(4097, 4096), 8192);
    CHECK_EQ(ts_align_down(17, 16), 16);
    CHECK_EQ(ts_align_down(15, 16), 0);
    CHECK_EQ(ts_align_down(TS_SEGMENT_SIZE + 5, TS_SEGMENT_SIZE), TS_SEGMENT_SIZE);
}

TEST(is_pow2) {
    CHECK(!ts_is_pow2(0));
    CHECK(ts_is_pow2(1));
    CHECK(ts_is_pow2(2));
    CHECK(!ts_is_pow2(3));
    CHECK(!ts_is_pow2(24));
    CHECK(ts_is_pow2((size_t)1 << 63));
}

TEST(bsr) {
    CHECK_EQ(ts_bsr(1), 0);
    CHECK_EQ(ts_bsr(2), 1);
    CHECK_EQ(ts_bsr(3), 1);
    CHECK_EQ(ts_bsr(1024), 10);
    CHECK_EQ(ts_bsr(1025), 10);
    CHECK_EQ(ts_bsr(SIZE_MAX), 63);
    for (unsigned i = 0; i < 64; i++)
        CHECK_EQ(ts_bsr((size_t)1 << i), i);
}

TEST(segment_mask_finds_segment_start) {
    uintptr_t seg = (uintptr_t)7 * TS_SEGMENT_SIZE;
    CHECK_EQ(seg & TS_SEGMENT_MASK, 0);
    CHECK_EQ((seg + 12345) & ~(uintptr_t)TS_SEGMENT_MASK, seg);
    CHECK_EQ((seg + TS_SEGMENT_SIZE - 1) & ~(uintptr_t)TS_SEGMENT_MASK, seg);
    CHECK_EQ(((seg + 3 * TS_PAGE_SIZE + 9) - seg) >> TS_PAGE_SHIFT, 3);
}
