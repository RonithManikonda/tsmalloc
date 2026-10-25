#include "test.h"

#include "segment.h"
#include "size_class.h"
#include "tsmalloc.h"

#include <errno.h>

static int from_segment(const void *p) {
    return *(const uint32_t *)ts_ptr_segment((const char *)p - 1) == TS_KIND_SEGMENT;
}

TEST(every_alignment_and_size) {
    static const size_t sizes[] = {1, 8, 16, 24, 100, 1000, 4096, 5000,
                                   TS_SMALL_MAX, TS_SMALL_MAX + 1, 300000};
    for (size_t align = 8; align <= ((size_t)1 << 20); align <<= 1) {
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            size_t size = sizes[si];
            void *p = NULL;
            REQUIRE(ts_posix_memalign(&p, align, size) == 0);
            REQUIRE(p != NULL);
            CHECK_EQ((uintptr_t)p & (align - 1), 0);
            CHECK(ts_usable_size(p) >= size);
            memset(p, 0x3C, size);

            void *q = ts_aligned_alloc(align, size);
            REQUIRE(q != NULL);
            CHECK_EQ((uintptr_t)q & (align - 1), 0);
            memset(q, 0xC3, size);
            CHECK(((unsigned char *)p)[size - 1] == 0x3C);
            ts_free(q);
            ts_free(p);
        }
    }
}

/* Up to 4 KiB, small aligned requests come from ordinary pages. Fill
 * several pages' worth so blocks from page 0 (shifted by the header) and
 * later pages are both checked. */
TEST(small_alignments_use_size_classes) {
    enum { N = 300 };
    static void *blocks[N];
    for (size_t align = 32; align <= 4096; align <<= 1) {
        size_t size = align / 2 + 3;
        for (int i = 0; i < N; i++) {
            blocks[i] = ts_aligned_alloc(align, size);
            REQUIRE(blocks[i] != NULL);
            CHECK_EQ((uintptr_t)blocks[i] & (align - 1), 0);
            CHECK(from_segment(blocks[i]));
            CHECK_EQ(ts_usable_size(blocks[i]) % align, 0);
        }
        for (int i = 0; i < N; i++)
            ts_free(blocks[i]);
    }
}

TEST(aligned_class_picks_smallest_multiple) {
    CHECK_EQ(ts_class_size(ts_size_to_class_aligned(1, 16)), 16);
    CHECK_EQ(ts_class_size(ts_size_to_class_aligned(1, 32)), 32);
    CHECK_EQ(ts_class_size(ts_size_to_class_aligned(33, 32)), 64);
    CHECK_EQ(ts_class_size(ts_size_to_class_aligned(130, 32)), 160);
    CHECK_EQ(ts_class_size(ts_size_to_class_aligned(130, 64)), 192);
    CHECK_EQ(ts_class_size(ts_size_to_class_aligned(5000, 4096)), 8192);
    CHECK_EQ(ts_size_to_class_aligned(TS_SMALL_MAX + 1, 16), TS_NUM_CLASSES);
    for (size_t align = 16; align <= 4096; align <<= 1)
        for (size_t size = 1; size <= TS_SMALL_MAX; size += 61) {
            unsigned cls = ts_size_to_class_aligned(size, align);
            REQUIRE(cls < TS_NUM_CLASSES);
            CHECK(ts_class_size(cls) >= size);
            CHECK_EQ(ts_class_size(cls) % align, 0);
        }
}

TEST(bad_alignments_are_einval) {
    void *p = (void *)1;
    CHECK_EQ(ts_posix_memalign(&p, 0, 16), EINVAL);
    CHECK_EQ(ts_posix_memalign(&p, 4, 16), EINVAL); /* below sizeof(void *) */
    CHECK_EQ(ts_posix_memalign(&p, 48, 16), EINVAL);
    CHECK(p == (void *)1);

    errno = 0;
    CHECK(ts_aligned_alloc(0, 16) == NULL);
    CHECK_EQ(errno, EINVAL);
    errno = 0;
    CHECK(ts_aligned_alloc(96, 16) == NULL);
    CHECK_EQ(errno, EINVAL);
}

TEST(aligned_alloc_accepts_tiny_alignments) {
    for (size_t align = 1; align < 16; align <<= 1) {
        void *p = ts_aligned_alloc(align, 10);
        REQUIRE(p != NULL);
        CHECK(from_segment(p));
        ts_free(p);
    }
}

TEST(huge_alignment_fails_cleanly) {
    void *p = (void *)1;
    CHECK_EQ(ts_posix_memalign(&p, (size_t)1 << 40, 16), ENOMEM);
    CHECK(p == (void *)1);
}
