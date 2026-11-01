#include "test.h"

#include "os.h"
#include "internal.h"
#include "tsmalloc.h"

#include <errno.h>

static void fill(unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(i * 31 + seed);
}

static int check_fill(const unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(i * 31 + seed))
            return 0;
    return 1;
}

TEST(large_sizes_are_aligned_and_fully_usable) {
    static const size_t sizes[] = {TS_MEDIUM_MAX + 1, 600000, 1 << 20, (1 << 20) + 3,
                                   4 << 20, (8 << 20) + 7};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        unsigned char *p = ts_malloc(sizes[i]);
        REQUIRE(p != NULL);
        CHECK(((uintptr_t)p & (TS_ALIGN - 1)) == 0);
        size_t usable = ts_usable_size(p);
        CHECK(usable >= sizes[i]);
        /* The header sits at the start of the mapping, and the usable size
         * runs to the end of its last page. */
        CHECK_EQ(((uintptr_t)p - 16) & (ts_os_page_size() - 1), 0);
        CHECK_EQ((usable + 16) % ts_os_page_size(), 0);
        fill(p, usable, (unsigned)i);
        CHECK(check_fill(p, usable, (unsigned)i));
        ts_free(p);
    }
}

TEST(small_and_medium_boundaries_still_work) {
    static const size_t edges[] = {TS_SMALL_MAX, TS_MEDIUM_MAX};
    for (size_t e = 0; e < 2; e++)
        for (size_t size = edges[e] - 32; size <= edges[e] + 32; size++) {
            unsigned char *p = ts_malloc(size);
            REQUIRE(p != NULL);
            CHECK(ts_usable_size(p) >= size);
            fill(p, ts_usable_size(p), 7);
            ts_free(p);
        }
}

TEST(calloc_large_is_zero_and_overflow_fails) {
    unsigned char *p = ts_calloc(3, 1 << 20);
    REQUIRE(p != NULL);
    for (size_t i = 0; i < (3u << 20); i += 4093)
        CHECK(p[i] == 0);
    ts_free(p);

    errno = 0;
    CHECK(ts_calloc(SIZE_MAX / 2, 3) == NULL);
    CHECK_EQ(errno, ENOMEM);
}

TEST(malloc_absurd_size_fails) {
    errno = 0;
    CHECK(ts_malloc(SIZE_MAX) == NULL);
    CHECK_EQ(errno, ENOMEM);
    CHECK(ts_malloc(SIZE_MAX - 4096) == NULL);
    CHECK(ts_malloc((size_t)1 << 62) == NULL);
}

TEST(realloc_small_to_large_and_back) {
    unsigned char *p = ts_malloc(100);
    REQUIRE(p != NULL);
    fill(p, 100, 1);

    p = ts_realloc(p, 50000);
    REQUIRE(p != NULL);
    CHECK(check_fill(p, 100, 1));
    fill(p, 50000, 2);

    p = ts_realloc(p, 3 << 20);
    REQUIRE(p != NULL);
    CHECK(check_fill(p, 50000, 2));

    p = ts_realloc(p, 64);
    REQUIRE(p != NULL);
    CHECK(check_fill(p, 64, 2));
    ts_free(p);
}

TEST(realloc_large_shrink_by_little_stays_in_place) {
    unsigned char *p = ts_malloc(1 << 20);
    REQUIRE(p != NULL);
    fill(p, 1 << 20, 3);
    unsigned char *q = ts_realloc(p, (1 << 20) - 1000);
    CHECK(q == p);
    CHECK(check_fill(q, (1 << 20) - 1000, 3));
    ts_free(q);
}

TEST(aligned_large) {
    for (size_t align = 16; align <= ((size_t)1 << 21); align <<= 1) {
        size_t size = TS_MEDIUM_MAX + 100;
        unsigned char *p = ts_aligned_alloc(align, size);
        REQUIRE(p != NULL);
        CHECK(((uintptr_t)p & (align - 1)) == 0);
        CHECK(ts_usable_size(p) >= size);
        fill(p, size, 9);
        ts_free(p);
    }
}

TEST(aligned_small_with_big_alignment) {
    void *p = NULL;
    CHECK_EQ(ts_posix_memalign(&p, 1 << 16, 48), 0);
    REQUIRE(p != NULL);
    CHECK(((uintptr_t)p & 0xFFFF) == 0);
    CHECK(ts_usable_size(p) >= 48);
    ts_free(p);
}
