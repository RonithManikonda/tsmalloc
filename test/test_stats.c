#include "test.h"

#include "huge.h"
#include "stats.h"
#include "tsmalloc.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Built twice: linked against the normal library, and as test_stats_on
 * against a copy with TS_STATS defined. */

#define MIB ((size_t)1 << 20)

static char *print_to_string(char *buf, size_t cap) {
    FILE *f = tmpfile();
    if (f == NULL)
        return NULL;
    ts_stats_print(f);
    rewind(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

#ifdef TS_STATS

TEST(malloc_and_free_are_counted) {
    uint64_t m = ts_stat_get(TS_STAT_MALLOC), f = ts_stat_get(TS_STAT_FREE);
    void *blocks[100];
    for (size_t i = 0; i < 100; i++)
        blocks[i] = ts_malloc(32);
    for (size_t i = 0; i < 100; i++)
        ts_free(blocks[i]);
    CHECK_EQ(ts_stat_get(TS_STAT_MALLOC) - m, 100);
    CHECK_EQ(ts_stat_get(TS_STAT_FREE) - f, 100);
}

TEST(free_null_is_not_counted) {
    uint64_t f = ts_stat_get(TS_STAT_FREE);
    ts_free(NULL);
    CHECK_EQ(ts_stat_get(TS_STAT_FREE) - f, 0);
}

TEST(every_allocating_call_counts_once) {
    uint64_t m = ts_stat_get(TS_STAT_MALLOC);
    void *a = ts_calloc(4, 16);
    void *b = ts_aligned_alloc(64, 64);
    void *c = ts_aligned_alloc(8, 64); /* takes the plain malloc route */
    void *d = NULL;
    CHECK_EQ(ts_posix_memalign(&d, 256, 256), 0);
    CHECK_EQ(ts_stat_get(TS_STAT_MALLOC) - m, 4);
    ts_free(a);
    ts_free(b);
    ts_free(c);
    ts_free(d);
}

static void *free_it(void *p) {
    ts_free(p);
    return NULL;
}

TEST(free_from_another_thread_is_remote) {
    void *p = ts_malloc(48);
    REQUIRE(p != NULL);
    uint64_t r = ts_stat_get(TS_STAT_REMOTE_FREE);
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, free_it, p) == 0);
    pthread_join(t, NULL);
    CHECK_EQ(ts_stat_get(TS_STAT_REMOTE_FREE) - r, 1);
}

TEST(huge_counters_follow_the_cache) {
    ts_huge_cache_flush();
    uint64_t map = ts_stat_get(TS_STAT_HUGE_MAP);
    uint64_t hit = ts_stat_get(TS_STAT_HUGE_CACHE_HIT);
    uint64_t unmap = ts_stat_get(TS_STAT_HUGE_UNMAP);

    void *p = ts_malloc(2 * MIB);
    CHECK_EQ(ts_stat_get(TS_STAT_HUGE_MAP) - map, 1);
    ts_free(p);
    CHECK_EQ(ts_stat_get(TS_STAT_HUGE_UNMAP) - unmap, 0);
    p = ts_malloc(2 * MIB);
    CHECK_EQ(ts_stat_get(TS_STAT_HUGE_CACHE_HIT) - hit, 1);
    CHECK_EQ(ts_stat_get(TS_STAT_HUGE_MAP) - map, 1);
    ts_free(p);
    ts_huge_cache_flush();
    CHECK_EQ(ts_stat_get(TS_STAT_HUGE_UNMAP) - unmap, 1);
}

static void *page_churn(void *arg) {
    (void)arg;
    void *blocks[400];
    for (size_t i = 0; i < 400; i++)
        blocks[i] = ts_malloc(1024);
    for (size_t i = 0; i < 400; i++)
        ts_free(blocks[i]);
    return NULL;
}

TEST(pages_and_segments_are_counted) {
    uint64_t pa = ts_stat_get(TS_STAT_PAGE_ALLOC);
    uint64_t pr = ts_stat_get(TS_STAT_PAGE_RETIRE);
    uint64_t sm = ts_stat_get(TS_STAT_SEGMENT_MAP);
    uint64_t su = ts_stat_get(TS_STAT_SEGMENT_UNMAP);
    /* A new thread has no segments, and gives them back when it exits. */
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, page_churn, NULL) == 0);
    pthread_join(t, NULL);
    CHECK(ts_stat_get(TS_STAT_PAGE_ALLOC) - pa >= 6);
    CHECK(ts_stat_get(TS_STAT_PAGE_RETIRE) - pr >= 1);
    CHECK(ts_stat_get(TS_STAT_SEGMENT_MAP) - sm >= 1);
    CHECK(ts_stat_get(TS_STAT_SEGMENT_UNMAP) - su >= 1);
}

TEST(print_lists_the_counters) {
    char buf[1024];
    REQUIRE(print_to_string(buf, sizeof buf) != NULL);
    CHECK(strstr(buf, "mallocs") != NULL);
    CHECK(strstr(buf, "remote frees") != NULL);
    CHECK(strstr(buf, "huge cache hits") != NULL);
}

#else

TEST(counters_stay_zero_without_stats) {
    void *p = ts_malloc(64);
    REQUIRE(p != NULL);
    ts_free(p);
    for (int i = 0; i < TS_STAT_COUNT; i++)
        CHECK_EQ(ts_stat_get((ts_stat_t)i), 0);
}

TEST(print_says_stats_are_off) {
    char buf[256];
    REQUIRE(print_to_string(buf, sizeof buf) != NULL);
    CHECK(strstr(buf, "not compiled in") != NULL);
}

#endif
