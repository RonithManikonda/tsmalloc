#include "test.h"

#include "tsmalloc.h"

#include <errno.h>

static int all_bytes_are(const unsigned char *p, size_t n, unsigned char v) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

TEST(malloc_free_various_sizes) {
    static const size_t sizes[] = {1, 8, 16, 24, 100, 1000, 4096, 16384,
                                   100000, 1 << 20, 8 << 20};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        unsigned char *p = ts_malloc(sizes[i]);
        REQUIRE(p != NULL);
        CHECK(((uintptr_t)p & 15) == 0);
        CHECK(ts_usable_size(p) >= sizes[i]);
        memset(p, 0xAB, sizes[i]);
        CHECK(all_bytes_are(p, sizes[i], 0xAB));
        ts_free(p);
    }
}

TEST(malloc_zero_is_freeable) {
    void *p = ts_malloc(0);
    ts_free(p); /* NULL or a unique pointer; either way free must accept it */
}

TEST(free_null_is_noop) {
    ts_free(NULL);
    CHECK_EQ(ts_usable_size(NULL), 0);
}

TEST(calloc_zeroes) {
    unsigned char *p = ts_calloc(1000, 7);
    REQUIRE(p != NULL);
    CHECK(all_bytes_are(p, 7000, 0));
    ts_free(p);
}

TEST(realloc_preserves_contents) {
    unsigned char *p = ts_realloc(NULL, 32);
    REQUIRE(p != NULL);
    for (int i = 0; i < 32; i++)
        p[i] = (unsigned char)i;

    p = ts_realloc(p, 100000);
    REQUIRE(p != NULL);
    for (int i = 0; i < 32; i++)
        CHECK_EQ(p[i], i);

    p = ts_realloc(p, 8);
    REQUIRE(p != NULL);
    for (int i = 0; i < 8; i++)
        CHECK_EQ(p[i], i);
    ts_free(p);
}

TEST(aligned_alloc_alignment) {
    for (size_t align = 16; align <= (1 << 16); align <<= 1) {
        void *p = ts_aligned_alloc(align, align * 3);
        REQUIRE(p != NULL);
        CHECK(((uintptr_t)p & (align - 1)) == 0);
        ts_free(p);
    }
}

TEST(posix_memalign_rejects_bad_alignment) {
    void *p = (void *)1;
    CHECK_EQ(ts_posix_memalign(&p, 24, 64), EINVAL); /* not a power of two */
    CHECK(p == (void *)1);                          /* untouched on error */

    CHECK_EQ(ts_posix_memalign(&p, 64, 100), 0);
    REQUIRE(p != NULL);
    CHECK(((uintptr_t)p & 63) == 0);
    ts_free(p);
}
