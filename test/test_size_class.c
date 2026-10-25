#include "test.h"

#include "size_class.h"

TEST(class_sizes_are_increasing_multiples_of_16) {
    for (unsigned c = 0; c < TS_NUM_CLASSES; c++) {
        CHECK_EQ(ts_class_size(c) % TS_ALIGN, 0);
        if (c > 0)
            CHECK(ts_class_size(c) > ts_class_size(c - 1));
    }
    CHECK_EQ(ts_class_size(0), 16);
    CHECK_EQ(ts_class_size(TS_NUM_CLASSES - 1), TS_SMALL_MAX);
}

TEST(every_size_maps_to_smallest_fitting_class) {
    for (size_t size = 1; size <= TS_SMALL_MAX; size++) {
        unsigned c = ts_size_to_class(size);
        REQUIRE(c < TS_NUM_CLASSES);
        CHECK(ts_class_size(c) >= size);
        if (c > 0)
            CHECK(ts_class_size(c - 1) < size);
    }
    CHECK_EQ(ts_size_to_class(0), 0);
}

TEST(waste_is_at_most_a_quarter_above_128) {
    /* Below 128 B the 16 B spacing dominates (17 B -> 32 B), which is the
     * usual trade for keeping small classes few and aligned. */
    for (size_t size = 128; size <= TS_SMALL_MAX; size++) {
        size_t got = ts_class_size(ts_size_to_class(size));
        CHECK((got - size) * 4 <= size);
    }
}

TEST(lut_agrees_with_bit_math) {
    for (size_t size = 129; size <= TS_LUT_MAX; size++)
        CHECK_EQ(ts_size_to_class(size), ts_size_to_class_large(size));
}

TEST(power_of_two_boundaries) {
    CHECK_EQ(ts_class_size(ts_size_to_class(128)), 128);
    CHECK_EQ(ts_class_size(ts_size_to_class(129)), 160);
    CHECK_EQ(ts_class_size(ts_size_to_class(1024)), 1024);
    CHECK_EQ(ts_class_size(ts_size_to_class(1025)), 1280);
    CHECK_EQ(ts_class_size(ts_size_to_class(8193)), 10240);
    CHECK_EQ(ts_size_to_class(TS_SMALL_MAX), TS_NUM_CLASSES - 1);
}
