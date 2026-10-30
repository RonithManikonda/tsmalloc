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

/* ---- ts_thread_id ---------------------------------------------------------- */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

enum { ID_THREADS = 8 };
static uintptr_t seen_ids[ID_THREADS];
static _Atomic int arrived; /* no pthread_barrier_t on macOS */

static void *record_id(void *arg) {
    size_t i = (size_t)(uintptr_t)arg;
    seen_ids[i] = ts_thread_id();
    /* Stay alive until everyone has recorded theirs: ids are only unique
     * among threads that exist at the same time. */
    atomic_fetch_add(&arrived, 1);
    while (atomic_load(&arrived) < ID_THREADS)
        sched_yield();
    return NULL;
}

TEST(thread_id_is_stable_and_nonzero) {
    uintptr_t a = ts_thread_id();
    CHECK(a != 0);
    CHECK_EQ(ts_thread_id(), a);
#if defined(__APPLE__) || (defined(__linux__) && defined(__x86_64__))
    /* On these targets it is pthread_self() read without the call. */
    CHECK_EQ(a, (uintptr_t)pthread_self());
#endif
}

TEST(thread_id_differs_between_live_threads) {
    pthread_t t[ID_THREADS];
    for (size_t i = 0; i < ID_THREADS; i++)
        REQUIRE(pthread_create(&t[i], NULL, record_id, (void *)(uintptr_t)i) == 0);
    for (size_t i = 0; i < ID_THREADS; i++)
        pthread_join(t[i], NULL);
    for (size_t i = 0; i < ID_THREADS; i++) {
        CHECK(seen_ids[i] != 0);
        CHECK(seen_ids[i] != ts_thread_id());
        for (size_t j = i + 1; j < ID_THREADS; j++)
            CHECK(seen_ids[i] != seen_ids[j]);
    }
}
