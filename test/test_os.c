#include "test.h"

#include "os.h"

static int is_pow2(size_t x) { return x != 0 && (x & (x - 1)) == 0; }

TEST(page_size_is_sane) {
    size_t ps = ts_os_page_size();
    CHECK(is_pow2(ps));
    CHECK(ps >= 4096);
    CHECK_EQ(ts_os_page_size(), ps); /* cached value is stable */
}

TEST(alloc_is_zeroed_and_writable) {
    size_t size = 3 * ts_os_page_size() + 100; /* not a page multiple */
    unsigned char *p = ts_os_alloc(size);
    REQUIRE(p != NULL);
    CHECK(((uintptr_t)p & (ts_os_page_size() - 1)) == 0);
    for (size_t i = 0; i < size; i++)
        CHECK(p[i] == 0);
    memset(p, 0x5A, size);
    ts_os_free(p, size);
}

TEST(alloc_huge_size_fails) {
    CHECK(ts_os_alloc(SIZE_MAX) == NULL);
    CHECK(ts_os_alloc(SIZE_MAX - 10) == NULL);
    CHECK(ts_os_alloc_aligned(SIZE_MAX - 4096, (size_t)1 << 22) == NULL);
}

TEST(alloc_aligned_various) {
    for (size_t align = 4096; align <= ((size_t)1 << 25); align <<= 1) {
        for (int round = 0; round < 8; round++) {
            size_t size = align + (size_t)round * ts_os_page_size();
            unsigned char *p = ts_os_alloc_aligned(size, align);
            REQUIRE(p != NULL);
            CHECK(((uintptr_t)p & (align - 1)) == 0);
            p[0] = 1;
            p[size - 1] = 2; /* whole range is mapped */
            ts_os_free(p, size);
        }
    }
}

TEST(aligned_regions_do_not_overlap) {
    enum { N = 32 };
    size_t seg = (size_t)4 << 20;
    unsigned char *regions[N];
    for (int i = 0; i < N; i++) {
        regions[i] = ts_os_alloc_aligned(seg, seg);
        REQUIRE(regions[i] != NULL);
        CHECK(((uintptr_t)regions[i] & (seg - 1)) == 0);
        regions[i][0] = (unsigned char)i;
        regions[i][seg - 1] = (unsigned char)i;
    }
    for (int i = 0; i < N; i++) {
        CHECK_EQ(regions[i][0], i);
        CHECK_EQ(regions[i][seg - 1], i);
        ts_os_free(regions[i], seg);
    }
}

TEST(repeated_aligned_alloc_does_not_leak_mappings) {
    /* If the head/tail trimming leaked, 4096 * 8 MiB = 32 GiB of address
     * space would pile up and mmap would eventually fail. */
    size_t seg = (size_t)4 << 20;
    for (int i = 0; i < 4096; i++) {
        void *p = ts_os_alloc_aligned(seg, seg);
        REQUIRE(p != NULL);
        ts_os_free(p, seg);
    }
}

TEST(decommit_keeps_range_usable) {
    size_t ps = ts_os_page_size();
    size_t size = 16 * ps;
    unsigned char *p = ts_os_alloc(size);
    REQUIRE(p != NULL);
    memset(p, 0xEE, size);

    ts_os_decommit(p, size);
    /* Contents are now unspecified (old bytes or zeros), but the memory
     * must stay mapped and writable. */
    for (size_t i = 0; i < size; i += ps)
        CHECK(p[i] == 0xEE || p[i] == 0);
    memset(p, 0x11, size);
    CHECK(p[size - 1] == 0x11);

    /* A range smaller than a page, or straddling one boundary, is a no-op
     * on the partial pages and must not crash. */
    ts_os_decommit(p + 1, ps - 2);
    ts_os_decommit(p + ps / 2, ps);
    CHECK(p[0] == 0x11);
    ts_os_free(p, size);
}
