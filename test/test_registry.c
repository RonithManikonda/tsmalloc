#include "test.h"

#include "huge.h"
#include "registry.h"
#include "segment.h"
#include "tsmalloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MIB ((size_t)1 << 20)

TEST(null_and_impossible_addresses_are_not_ours) {
    CHECK(!ts_is_ours(NULL));
    CHECK(!ts_is_ours((void *)((uintptr_t)1 << TS_REGISTRY_ADDRESS_BITS)));
    CHECK(!ts_is_ours((void *)UINTPTR_MAX));
}

TEST(foreign_pointers_are_not_ours) {
    int on_stack = 0;
    CHECK(!ts_is_ours(&on_stack));
    /* Make sure there is something registered to get wrong. */
    void *ours = ts_malloc(64);
    static const size_t sizes[] = {16, 1024, 64 * 1024, MIB, 8 * MIB};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        void *p = malloc(sizes[i]);
        REQUIRE(p != NULL);
        CHECK(!ts_is_ours(p));
        free(p);
    }
    ts_free(ours);
}

TEST(blocks_of_every_kind_are_ours) {
    static const size_t sizes[] = {1, 64, 1024, TS_SMALL_MAX, 100 * 1024, TS_MEDIUM_MAX,
                                   TS_MEDIUM_MAX + 1, 3 * MIB, 16 * MIB};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        void *p = ts_malloc(sizes[i]);
        REQUIRE(p != NULL);
        CHECK(ts_is_ours(p));
        ts_free(p);
    }
}

TEST(aligned_blocks_are_ours) {
    for (size_t align = 16; align <= 32 * MIB; align <<= 1) {
        void *p = ts_aligned_alloc(align, TS_MEDIUM_MAX + 64);
        REQUIRE(p != NULL);
        CHECK(ts_is_ours(p));
        ts_free(p);
    }
}

TEST(calloc_huge_is_ours) {
    void *p = ts_calloc(2, MIB);
    REQUIRE(p != NULL);
    CHECK(ts_is_ours(p));
    ts_free(p);
}

TEST(freed_huge_block_is_not_ours_even_while_cached) {
    ts_huge_cache_flush();
    void *p = ts_malloc(2 * MIB);
    REQUIRE(p != NULL);
    CHECK(ts_is_ours(p));
    ts_free(p);
    CHECK(!ts_is_ours(p));

    /* Reuse registers it again. */
    void *q = ts_malloc(2 * MIB);
    CHECK(q == p);
    CHECK(ts_is_ours(q));
    ts_free(q);
    ts_huge_cache_flush();
    CHECK(!ts_is_ours(p));
}

TEST(huge_block_address_must_be_exact) {
    /* 1 MiB used in a 4 MiB chunk: the chunk's bit is set, but only one
     * address in it is a block. These are compared, never dereferenced. */
    char *p = ts_malloc(MIB);
    REQUIRE(p != NULL);
    CHECK(ts_is_ours(p));
    CHECK(!ts_is_ours(p + 16));
    CHECK(!ts_is_ours(p + 2 * MIB));
    ts_free(p);
    ts_huge_cache_flush();
}

/* Asks the OS for 64 KiB at `hint`: 1 if it got that address, 0 if it got
 * another, -1 if mmap failed. Without MAP_FIXED the hint is only a
 * preference, so this never replaces anything; it lands there only if the
 * range is free. */
static int could_map_at(char *hint) {
    size_t len = 64 * 1024;
    void *q = mmap(hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (q == MAP_FAILED)
        return -1;
    munmap(q, len);
    return (char *)q >= hint && (char *)q < hint + len;
}

TEST(huge_block_keeps_the_rest_of_its_chunk) {
    /* While a chunk's bit is set, nobody else may map anything in it, or
     * ts_is_ours on their pointer could read our header just as we unmap it. */
    char *p = ts_malloc(MIB);
    REQUIRE(p != NULL);
    char *chunk = (char *)ts_ptr_segment(p - 1);
    CHECK_EQ(could_map_at(chunk + 2 * MIB), 0);
    CHECK_EQ(could_map_at(chunk + TS_SEGMENT_SIZE - 64 * 1024), 0);
    ts_free(p);
    ts_huge_cache_flush();

    /* The same after a shrink gives pages back. */
    p = ts_malloc(6 * MIB);
    REQUIRE(p != NULL);
    REQUIRE(ts_realloc(p, MIB) == p);
    chunk = (char *)ts_ptr_segment(p - 1);
    CHECK_EQ(could_map_at(chunk + 2 * MIB), 0);
    CHECK_EQ(could_map_at(chunk + TS_SEGMENT_SIZE + MIB), 1);
    ts_free(p);
    ts_huge_cache_flush();
}

TEST(shrunk_huge_block_stays_ours) {
    char *p = ts_malloc(6 * MIB);
    REQUIRE(p != NULL);
    char *q = ts_realloc(p, MIB);
    CHECK(q == p);
    CHECK(ts_is_ours(q));
    ts_free(q);
    ts_huge_cache_flush();
}

TEST(pointer_just_past_a_segment_is_not_ours) {
    char *p = ts_malloc(64);
    REQUIRE(p != NULL);
    ts_segment_t *seg = ts_ptr_segment(p);
    /* p - 1 masks to our segment here, whatever is mapped after it. */
    CHECK(!ts_is_ours((char *)seg + TS_SEGMENT_SIZE));
    CHECK(!ts_is_ours(seg));
    ts_free(p);
}

static void *segment_owner(void *out) {
    void *p = ts_malloc(TS_SMALL_MAX);
    memcpy(out, &p, sizeof p);
    ts_free(p);
    return NULL;
}

TEST(segments_are_forgotten_when_unmapped) {
    /* The thread's only segment holds nothing when it exits, so it goes back
     * to the OS with the thread. */
    void *p = NULL;
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, segment_owner, &p) == 0);
    pthread_join(t, NULL);
    REQUIRE(p != NULL);
    CHECK(!ts_is_ours(p));
}

/* Threads map and unmap regions while checking their own live ones. Bits for
 * neighbouring chunks share a word, so lost updates would show up here. */
#define WORKERS 8
static atomic_int wrong;

static void *registry_worker(void *arg) {
    unsigned x = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    void *live[8] = {0};
    int rounds = 600;
    const char *s = getenv("TS_TEST_SCALE");
    if (s != NULL && atoi(s) > 0)
        rounds *= atoi(s);
    for (int r = 0; r < rounds; r++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        size_t slot = x % 8;
        if (live[slot] != NULL) {
            if (!ts_is_ours(live[slot]))
                atomic_fetch_add(&wrong, 1);
            ts_free(live[slot]);
        }
        /* Oversized for the cache, so each one really maps and unmaps. */
        live[slot] = ts_malloc(TS_HUGE_CACHE_MAX_SIZE + 1 + (x >> 8) % MIB);
        if (live[slot] == NULL || !ts_is_ours(live[slot]))
            atomic_fetch_add(&wrong, 1);
    }
    for (size_t i = 0; i < 8; i++)
        ts_free(live[i]);
    return NULL;
}

TEST(registry_survives_concurrent_churn) {
    pthread_t t[WORKERS];
    for (uintptr_t i = 0; i < WORKERS; i++)
        REQUIRE(pthread_create(&t[i], NULL, registry_worker, (void *)(i + 1)) == 0);
    for (size_t i = 0; i < WORKERS; i++)
        pthread_join(t[i], NULL);
    CHECK_EQ(atomic_load(&wrong), 0);
}
