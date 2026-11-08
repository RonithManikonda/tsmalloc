#include "test.h"

#include "huge.h"
#include "internal.h"
#include "os.h"
#include "tsmalloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MIB ((size_t)1 << 20)

static int test_scale(void) {
    const char *s = getenv("TS_TEST_SCALE");
    return s != NULL && atoi(s) > 0 ? atoi(s) : 1;
}

static size_t cached_count(void) {
    size_t count, bytes, hot;
    ts_huge_cache_info(&count, &bytes, &hot);
    return count;
}

static size_t cached_bytes(void) {
    size_t count, bytes, hot;
    ts_huge_cache_info(&count, &bytes, &hot);
    return bytes;
}

static size_t cached_hot_bytes(void) {
    size_t count, bytes, hot;
    ts_huge_cache_info(&count, &bytes, &hot);
    return hot;
}

TEST(freed_mapping_is_reused) {
    ts_huge_cache_flush();
    char *p = ts_malloc(MIB);
    REQUIRE(p != NULL);
    memset(p, 0xAB, MIB);
    ts_free(p);
    CHECK_EQ(cached_count(), 1);

    char *q = ts_malloc(MIB);
    CHECK(q == p);
    CHECK_EQ(cached_count(), 0);
    /* The whole block has to be writable again after the decommit. */
    memset(q, 0xCD, MIB);
    CHECK_EQ((unsigned char)q[MIB - 1], 0xCD);
    ts_free(q);
    ts_huge_cache_flush();
}

TEST(mapping_too_small_is_skipped) {
    ts_huge_cache_flush();
    char *p = ts_malloc(MIB);
    REQUIRE(p != NULL);
    ts_free(p);
    char *q = ts_malloc(2 * MIB);
    REQUIRE(q != NULL);
    CHECK_EQ(cached_count(), 1);
    CHECK(ts_usable_size(q) >= 2 * MIB);
    ts_free(q);
    ts_huge_cache_flush();
}

TEST(mapping_much_too_big_is_skipped) {
    ts_huge_cache_flush();
    char *p = ts_malloc(8 * MIB);
    REQUIRE(p != NULL);
    ts_free(p);
    char *q = ts_malloc(MIB);
    REQUIRE(q != NULL);
    CHECK(q != p);
    CHECK_EQ(cached_count(), 1);
    ts_free(q);
    ts_huge_cache_flush();
}

TEST(slightly_bigger_mapping_is_used_whole) {
    ts_huge_cache_flush();
    char *p = ts_malloc(MIB + MIB / 8);
    REQUIRE(p != NULL);
    size_t usable = ts_usable_size(p);
    ts_free(p);
    char *q = ts_malloc(MIB);
    CHECK(q == p);
    CHECK_EQ(ts_usable_size(q), usable);
    ts_free(q);
    ts_huge_cache_flush();
}

TEST(calloc_never_recycles) {
    ts_huge_cache_flush();
    unsigned char *p = ts_malloc(2 * MIB);
    REQUIRE(p != NULL);
    memset(p, 0xFF, 2 * MIB);
    ts_free(p);

    unsigned char *q = ts_calloc(1, 2 * MIB);
    REQUIRE(q != NULL);
    CHECK_EQ(cached_count(), 1);
    for (size_t i = 0; i < 2 * MIB; i += 512)
        CHECK_EQ(q[i], 0);
    ts_free(q);
    ts_huge_cache_flush();
}

TEST(alignment_is_honoured_on_reuse) {
    ts_huge_cache_flush();
    void *p = ts_malloc(3 * MIB);
    REQUIRE(p != NULL);
    ts_free(p);

    /* Lead (1 MiB) + size has to fit the cached 3 MiB. */
    void *q = ts_aligned_alloc(MIB, 2 * MIB);
    REQUIRE(q != NULL);
    CHECK_EQ((uintptr_t)q & (MIB - 1), 0);
    CHECK_EQ(cached_count(), 0);
    CHECK(ts_usable_size(q) >= 2 * MIB);
    memset(q, 1, 2 * MIB);
    ts_free(q);

    /* It went back with its bigger lead and can serve a plain request. */
    CHECK_EQ(cached_count(), 1);
    void *r = ts_malloc(3 * MIB);
    CHECK(r == p);
    ts_free(r);
    ts_huge_cache_flush();
}

TEST(alignment_above_segment_size_bypasses_the_cache) {
    ts_huge_cache_flush();
    void *p = ts_aligned_alloc(8 * MIB, MIB);
    REQUIRE(p != NULL);
    CHECK_EQ((uintptr_t)p & (8 * MIB - 1), 0);
    ts_free(p);
    CHECK_EQ(cached_count(), 0);
}

TEST(oversized_mappings_go_back_to_the_os) {
    ts_huge_cache_flush();
    void *p = ts_malloc(TS_HUGE_CACHE_MAX_SIZE + MIB);
    REQUIRE(p != NULL);
    ts_free(p);
    CHECK_EQ(cached_count(), 0);
}

TEST(bucket_keeps_only_its_newest_mappings) {
    ts_huge_cache_flush();
    enum { N = TS_HUGE_BUCKET_SLOTS + 3 };
    void *blocks[N];
    for (size_t i = 0; i < N; i++) {
        blocks[i] = ts_malloc(MIB);
        REQUIRE(blocks[i] != NULL);
    }
    for (size_t i = 0; i < N; i++)
        ts_free(blocks[i]);
    CHECK_EQ(cached_count(), TS_HUGE_BUCKET_SLOTS);

    /* The survivors are the ones freed last. */
    void *again[TS_HUGE_BUCKET_SLOTS];
    for (size_t i = 0; i < TS_HUGE_BUCKET_SLOTS; i++) {
        void *p = again[i] = ts_malloc(MIB);
        int recent = 0;
        for (size_t j = N - TS_HUGE_BUCKET_SLOTS; j < N; j++)
            recent |= p == blocks[j];
        CHECK(recent);
    }
    CHECK_EQ(cached_count(), 0);
    for (size_t i = 0; i < TS_HUGE_BUCKET_SLOTS; i++)
        ts_free(again[i]);
    ts_huge_cache_flush();
}

TEST(total_bytes_stay_under_the_cap) {
    ts_huge_cache_flush();
    /* Every bucket from 32 MiB down, full: far more than the cap in total. */
    enum { N = 16 };
    void *blocks[N];
    for (size_t i = 0; i < N; i++) {
        blocks[i] = ts_malloc((size_t)32 * MIB - (i * MIB / 4));
        REQUIRE(blocks[i] != NULL);
    }
    for (size_t i = 0; i < N; i++) {
        ts_free(blocks[i]);
        CHECK(cached_bytes() <= TS_HUGE_CACHE_MAX_BYTES);
    }
    CHECK(cached_bytes() > 0);
    ts_huge_cache_flush();
    CHECK_EQ(cached_bytes(), 0);
    CHECK_EQ(cached_count(), 0);
}

TEST(only_the_newest_mappings_stay_resident) {
    ts_huge_cache_flush();
    enum { N = 8 };
    char *blocks[N];
    /* Each size is far enough from the next not to share a mapping. */
    for (size_t i = 0; i < N; i++) {
        blocks[i] = ts_malloc(8 * MIB + i * MIB);
        REQUIRE(blocks[i] != NULL);
        memset(blocks[i], 0x5A, 8 * MIB);
    }
    for (size_t i = 0; i < N; i++) {
        ts_free(blocks[i]);
        CHECK(cached_hot_bytes() <= TS_HUGE_HOT_BYTES);
    }
    CHECK(cached_hot_bytes() > 0);
    CHECK(cached_bytes() > cached_hot_bytes());

    /* Taking a mapping out, decommitted or not, leaves the sums consistent. */
    for (size_t i = 0; i < N; i++) {
        char *p = ts_malloc(8 * MIB + i * MIB);
        REQUIRE(p != NULL);
        memset(p, 0x77, 8 * MIB);
        CHECK_EQ((unsigned char)p[8 * MIB - 1], 0x77);
        blocks[i] = p;
    }
    for (size_t i = 0; i < N; i++)
        ts_free(blocks[i]);
    ts_huge_cache_flush();
    CHECK_EQ(cached_hot_bytes(), 0);
}

TEST(shrunk_mapping_caches_its_new_size) {
    ts_huge_cache_flush();
    char *p = ts_malloc(4 * MIB);
    REQUIRE(p != NULL);
    char *q = ts_realloc(p, 2 * MIB);
    CHECK(q == p);
    ts_free(q);
    CHECK(cached_bytes() < 3 * MIB);
    ts_huge_cache_flush();
}

/* Blocks allocated by one thread, freed by another, through the shared cache. */
#define WORKERS 8
#define SLOTS   6
static _Atomic(unsigned char *) shared[SLOTS];
static atomic_int failures;

static void *huge_worker(void *arg) {
    unsigned x = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    int rounds = 400 * test_scale();
    for (int r = 0; r < rounds; r++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        size_t size = TS_MEDIUM_MAX + 1 + x % (6 * MIB);
        unsigned char tag = (unsigned char)(x >> 24);
        unsigned char *p = ts_malloc(size);
        if (p == NULL) {
            atomic_fetch_add(&failures, 1);
            break;
        }
        /* Size and tag live in the block so whoever frees it can check. */
        memcpy(p, &size, sizeof size);
        p[sizeof size] = tag;
        p[size - 1] = tag;

        unsigned char *old = atomic_exchange(&shared[x % SLOTS], p);
        if (old != NULL) {
            size_t old_size;
            memcpy(&old_size, old, sizeof old_size);
            if (old[old_size - 1] != old[sizeof old_size])
                atomic_fetch_add(&failures, 1);
            ts_free(old);
        }
    }
    return NULL;
}

TEST(threads_share_the_cache) {
    ts_huge_cache_flush();
    pthread_t t[WORKERS];
    for (uintptr_t i = 0; i < WORKERS; i++)
        REQUIRE(pthread_create(&t[i], NULL, huge_worker, (void *)(i + 1)) == 0);
    for (size_t i = 0; i < WORKERS; i++)
        pthread_join(t[i], NULL);
    for (size_t i = 0; i < SLOTS; i++) {
        unsigned char *p = atomic_exchange(&shared[i], NULL);
        if (p != NULL)
            ts_free(p);
    }
    CHECK_EQ(atomic_load(&failures), 0);
    CHECK(cached_bytes() <= TS_HUGE_CACHE_MAX_BYTES);
    ts_huge_cache_flush();
}
