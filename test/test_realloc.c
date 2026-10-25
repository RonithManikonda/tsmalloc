#include "test.h"

#include "internal.h"
#include "os.h"
#include "size_class.h"
#include "tsmalloc.h"

#include <errno.h>

static void fill(unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(i * 7 + seed);
}

static int check_fill(const unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(i * 7 + seed))
            return 0;
    return 1;
}

TEST(calloc_clears_recycled_small_blocks) {
    /* Dirty a block, free it, and make sure calloc's copy comes back zero
     * even though it is the same memory. */
    unsigned char *p = ts_malloc(200);
    REQUIRE(p != NULL);
    memset(p, 0xEE, ts_usable_size(p));
    ts_free(p);
    unsigned char *q = ts_calloc(25, 8);
    REQUIRE(q != NULL);
    CHECK(q == p);
    for (size_t i = 0; i < 200; i++)
        CHECK_EQ(q[i], 0);
    ts_free(q);
}

TEST(calloc_overflow_sets_enomem) {
    errno = 0;
    CHECK(ts_calloc(SIZE_MAX, 2) == NULL);
    CHECK_EQ(errno, ENOMEM);
    errno = 0;
    CHECK(ts_calloc((size_t)1 << 32, (size_t)1 << 32) == NULL);
    CHECK_EQ(errno, ENOMEM);
    /* Zero of anything is fine and freeable. */
    void *z = ts_calloc(0, 100);
    ts_free(z);
}

TEST(realloc_null_is_malloc) {
    unsigned char *p = ts_realloc(NULL, 40);
    REQUIRE(p != NULL);
    CHECK(ts_usable_size(p) >= 40);
    ts_free(p);
}

TEST(realloc_zero_returns_minimal_block) {
    unsigned char *p = ts_malloc(500);
    REQUIRE(p != NULL);
    unsigned char *q = ts_realloc(p, 0);
    REQUIRE(q != NULL);
    CHECK_EQ(ts_usable_size(q), ts_class_size(0));
    ts_free(q);

    p = ts_malloc(1 << 20);
    q = ts_realloc(p, 0);
    REQUIRE(q != NULL);
    CHECK_EQ(ts_usable_size(q), ts_class_size(0));
    ts_free(q);
}

TEST(realloc_within_class_keeps_pointer) {
    for (unsigned cls = 0; cls < TS_NUM_CLASSES; cls++) {
        size_t bsize = ts_class_size(cls);
        size_t lo = cls == 0 ? 1 : ts_class_size(cls - 1) + 1;
        unsigned char *p = ts_malloc(lo);
        REQUIRE(p != NULL);
        fill(p, lo, cls);
        CHECK(ts_realloc(p, bsize) == p);  /* grow to the top of the class */
        CHECK(ts_realloc(p, lo) == p);     /* and back */
        CHECK(check_fill(p, lo, cls));
        ts_free(p);
    }
}

TEST(realloc_small_big_shrink_moves) {
    unsigned char *p = ts_malloc(4096);
    REQUIRE(p != NULL);
    fill(p, 64, 3);
    unsigned char *q = ts_realloc(p, 64);
    REQUIRE(q != NULL);
    CHECK(q != p);
    CHECK_EQ(ts_usable_size(q), 64);
    CHECK(check_fill(q, 64, 3));
    ts_free(q);
}

TEST(realloc_grow_through_classes_keeps_contents) {
    unsigned char *p = ts_malloc(1);
    REQUIRE(p != NULL);
    p[0] = 0x5A;
    size_t size = 1;
    while (size < (4 << 20)) {
        size_t next = size * 3 / 2 + 1;
        p = ts_realloc(p, next);
        REQUIRE(p != NULL);
        CHECK_EQ(p[0], 0x5A);
        CHECK(size < 2 || check_fill(p + 1, size - 1, 9));
        fill(p + 1, next - 1, 9);
        size = next;
    }
    ts_free(p);
}

TEST(realloc_large_shrink_unmaps_tail_in_place) {
    size_t ps = ts_os_page_size();
    unsigned char *p = ts_malloc(1 << 20);
    REQUIRE(p != NULL);
    fill(p, 1 << 20, 5);
    unsigned char *q = ts_realloc(p, 100000);
    CHECK(q == p);
    CHECK(check_fill(q, 100000, 5));
    size_t usable = ts_usable_size(q);
    CHECK(usable >= 100000 && usable < 100000 + ps);
    /* Growing back within the remaining pages also stays put. */
    CHECK(ts_realloc(q, usable) == q);
    ts_free(q);
}

TEST(realloc_large_to_small_moves) {
    unsigned char *p = ts_malloc(TS_SMALL_MAX * 2);
    REQUIRE(p != NULL);
    fill(p, 1000, 6);
    unsigned char *q = ts_realloc(p, 1000);
    REQUIRE(q != NULL);
    CHECK(q != p);
    CHECK_EQ(ts_usable_size(q), ts_class_size(ts_size_to_class(1000)));
    CHECK(check_fill(q, 1000, 6));
    ts_free(q);
}

TEST(realloc_failure_keeps_old_block) {
    unsigned char *p = ts_malloc(64);
    REQUIRE(p != NULL);
    fill(p, 64, 8);
    errno = 0;
    CHECK(ts_realloc(p, SIZE_MAX - 4096) == NULL);
    CHECK_EQ(errno, ENOMEM);
    CHECK(check_fill(p, 64, 8));
    ts_free(p);
}

TEST(usable_size_is_class_size_for_small) {
    for (size_t size = 1; size <= TS_SMALL_MAX; size += 37) {
        void *p = ts_malloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ(ts_usable_size(p), ts_class_size(ts_size_to_class(size)));
        ts_free(p);
    }
}
