#include "test.h"

#include "page.h"
#include "size_class.h"

/* Pages to try: page 0 (shrunk by the header) and an ordinary one. */
static const size_t page_indices[] = {0, 5};

static ts_page_t *setup(ts_segment_t *seg, size_t index, size_t bsize) {
    ts_page_t *page = &seg->pages[index];
    page->in_use = 1;
    ts_page_init(page, bsize);
    return page;
}

TEST(capacity_matches_usable_space) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    ts_page_t *page = setup(seg, 3, 48);
    CHECK_EQ(page->capacity, TS_PAGE_SIZE / 48);
    page = setup(seg, 0, 16);
    CHECK_EQ(page->capacity, ts_page_usable(page) / 16);
    CHECK(page->capacity < TS_PAGE_SIZE / 16);
    ts_segment_free(seg);
}

TEST(extension_is_lazy) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    ts_page_t *page = setup(seg, 2, 64);
    CHECK_EQ(page->reserved, 0);
    void *a = ts_page_alloc(page);
    REQUIRE(a != NULL);
    CHECK_EQ(page->reserved, TS_PAGE_EXTEND_BYTES / 64);
    CHECK(page->reserved < page->capacity);
    /* Address order within a chunk. */
    void *b = ts_page_alloc(page);
    CHECK((uint8_t *)b == (uint8_t *)a + 64);

    page = setup(seg, 3, 16384); /* block bigger than one chunk */
    CHECK(ts_page_alloc(page) != NULL);
    CHECK_EQ(page->reserved, 1);
    ts_segment_free(seg);
}

/* Fill a page completely, tagging each block with its id in every word, then
 * verify nothing overlapped and every block lies inside the page. */
static void fill_and_verify(ts_segment_t *seg, size_t index, unsigned cls) {
    size_t bsize = ts_class_size(cls);
    ts_page_t *page = setup(seg, index, bsize);
    uint8_t *lo = ts_page_start(seg, page);
    uint8_t *hi = lo + ts_page_usable(page);

    static uint8_t *blocks[TS_PAGE_SIZE / 16];
    size_t n = 0;
    void *p;
    while ((p = ts_page_alloc(page)) != NULL) {
        uint8_t *b = p;
        CHECK(b >= lo && b + bsize <= hi);
        CHECK_EQ((uintptr_t)b % TS_ALIGN, 0);
        CHECK_EQ((size_t)(b - lo) % bsize, 0);
        for (size_t w = 0; w < bsize / sizeof(size_t); w++)
            ((size_t *)b)[w] = n;
        blocks[n++] = b;
    }
    CHECK_EQ(n, page->capacity);
    CHECK_EQ(page->used, page->capacity);
    CHECK(ts_page_is_full(page));

    for (size_t i = 0; i < n; i++)
        for (size_t w = 0; w < bsize / sizeof(size_t); w++)
            if (((size_t *)blocks[i])[w] != i) {
                CHECK(!"block contents overwritten");
                return;
            }

    for (size_t i = 0; i < n; i++)
        ts_page_free(page, blocks[i]);
    CHECK(ts_page_is_empty(page));
    CHECK(!ts_page_is_full(page));

    /* Freed blocks come back (LIFO), and the cursor doesn't move again. */
    CHECK(ts_page_alloc(page) == blocks[n - 1]);
    CHECK_EQ(page->reserved, page->capacity);
}

TEST(every_class_fills_a_page_without_overlap) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    for (unsigned cls = 0; cls < TS_NUM_CLASSES; cls++)
        for (size_t i = 0; i < sizeof page_indices / sizeof page_indices[0]; i++)
            fill_and_verify(seg, page_indices[i] + cls % 3, cls);
    /* Page descriptors in the header were never touched by block writes. */
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++)
        CHECK_EQ(seg->pages[i].index, i);
    ts_segment_free(seg);
}

TEST(interleaved_alloc_free) {
    ts_segment_t *seg = ts_segment_alloc();
    REQUIRE(seg != NULL);
    ts_page_t *page = setup(seg, 1, 32);
    void *keep[100];
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 100; i++) {
            keep[i] = ts_page_alloc(page);
            REQUIRE(keep[i] != NULL);
        }
        for (int i = 0; i < 100; i += 2)
            ts_page_free(page, keep[i]);
        for (int i = 1; i < 100; i += 2)
            ts_page_free(page, keep[i]);
        CHECK(ts_page_is_empty(page));
    }
    /* Reuse kept the footprint at 100 blocks' worth of chunks. */
    CHECK(page->reserved <= 100 + TS_PAGE_EXTEND_BYTES / 32);
    ts_segment_free(seg);
}
