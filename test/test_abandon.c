#include "test.h"

#include "heap.h"
#include "tsmalloc.h"

#include <pthread.h>
#include <sched.h>

/* What happens to a thread's heap when the thread exits: whatever is free
 * goes back, segments with live blocks are abandoned (owner 0), and frees
 * into them still work. */

enum { N = 3000 };
static void *blocks[N];

static size_t remote_list_len(ts_page_t *page) {
    uintptr_t tf = atomic_load(&page->thread_free) & ~TS_TF_FLAGS;
    size_t n = 0;
    for (ts_block_t *b = (ts_block_t *)tf; b != NULL; b = b->next)
        n++;
    return n;
}

/* Blocks still live in the abandoned segments, not counting ones that sit
 * on a remote list waiting to be collected. */
static void count_live(ts_segment_t *seg, void *arg) {
    size_t *live = arg;
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++) {
        ts_page_t *page = &seg->pages[i];
        if (page->in_use)
            *live += page->used - remote_list_len(page);
    }
}

static size_t abandoned_live(void) {
    size_t live = 0;
    ts_abandoned_visit(count_live, &live);
    return live;
}

static void check_unowned(ts_segment_t *seg, void *arg) {
    int *bad = arg;
    if (atomic_load(&seg->thread_id) != 0 || seg->heap != NULL)
        (*bad)++;
    for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++)
        if (seg->pages[i].in_use && (seg->pages[i].queued ||
                                     (atomic_load(&seg->pages[i].thread_free) & TS_TF_FLAGS)))
            (*bad)++;
}

static void *alloc_n(void *arg) {
    size_t n = (size_t)(uintptr_t)arg;
    for (size_t i = 0; i < n; i++) {
        blocks[i] = ts_malloc(16 + (i % 64) * 16);
        if (blocks[i] != NULL)
            memset(blocks[i], (int)(i & 0xFF), 16);
    }
    return NULL;
}

static void *alloc_and_free_all(void *arg) {
    (void)arg;
    for (size_t i = 0; i < N; i++)
        blocks[i] = ts_malloc(256);
    for (size_t i = 0; i < N; i++)
        ts_free(blocks[i]);
    return NULL;
}

TEST(thread_that_freed_everything_leaves_nothing_behind) {
    size_t before = ts_abandoned_count();
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, alloc_and_free_all, NULL) == 0);
    pthread_join(t, NULL);
    CHECK_EQ(ts_abandoned_count(), before);
}

TEST(live_blocks_keep_their_segment_abandoned) {
    size_t before_segs = ts_abandoned_count();
    size_t before_live = abandoned_live();
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, alloc_n, (void *)(uintptr_t)N) == 0);
    pthread_join(t, NULL);

    CHECK(ts_abandoned_count() > before_segs);
    CHECK_EQ(abandoned_live(), before_live + N);
    int bad = 0;
    ts_abandoned_visit(check_unowned, &bad);
    CHECK_EQ(bad, 0);

    /* The blocks are intact, and freeing them from here goes remote: the
     * segments aren't ours, even though this thread has a heap. */
    void *mine = ts_malloc(32);
    for (size_t i = 0; i < N; i++) {
        CHECK_EQ(((unsigned char *)blocks[i])[15], i & 0xFF);
        CHECK(atomic_load(&ts_ptr_segment(blocks[i])->thread_id) != ts_thread_id());
        ts_free(blocks[i]);
    }
    ts_free(mine);
    CHECK_EQ(abandoned_live(), before_live);
}

/* A remote free into a full page goes to the heap's delayed list. If the
 * owner exits before collecting it, abandoning must still count the block
 * as freed. */
static struct {
    _Atomic int phase; /* 0 start, 1 blocks ready, 2 victim freed */
    void *victim;
    ts_page_t *victim_page;
} dl;

static void *fill_then_wait(void *arg) {
    (void)arg;
    size_t n = 0;
    /* 1 KiB blocks: a page holds 64, so the first pages are full. */
    while (n < 200)
        blocks[n++] = ts_malloc(1024);
    dl.victim = blocks[10];
    dl.victim_page = ts_ptr_page(ts_ptr_segment(dl.victim), dl.victim);
    atomic_store(&dl.phase, 1);
    while (atomic_load(&dl.phase) != 2)
        sched_yield();
    return NULL; /* exit without ever mallocing again */
}

TEST(abandon_collects_delayed_frees) {
    size_t before_live = abandoned_live();
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, fill_then_wait, NULL) == 0);
    while (atomic_load(&dl.phase) != 1)
        sched_yield();
    CHECK_EQ(atomic_load(&dl.victim_page->thread_free), TS_TF_FULL);
    ts_free(dl.victim);
    CHECK_EQ(atomic_load(&dl.victim_page->thread_free), 0);
    atomic_store(&dl.phase, 2);
    pthread_join(t, NULL);

    /* 199 still live; the victim was taken back during abandon. */
    CHECK_EQ(abandoned_live(), before_live + 199);
    for (size_t i = 0; i < 200; i++)
        if (i != 10)
            ts_free(blocks[i]);
    CHECK_EQ(abandoned_live(), before_live);
}

/* Stress: owners exit while several threads are still freeing their blocks,
 * including into full pages (the delayed path). Run under TSan to check the
 * BUSY handshake. TS_TEST_SCALE multiplies the rounds. */
enum { FREERS = 4, PER_ROUND = 1200 };

static struct {
    void *blocks[PER_ROUND];
    _Atomic int ready;
    _Atomic size_t next;
} sr;

static void *owner_alloc_and_exit(void *arg) {
    (void)arg;
    for (size_t i = 0; i < PER_ROUND; i++) {
        sr.blocks[i] = ts_malloc(512 + (i % 3) * 512);
        memset(sr.blocks[i], 0x5A, 16);
    }
    atomic_store_explicit(&sr.ready, 1, memory_order_release);
    return NULL;
}

static void *free_round(void *arg) {
    _Atomic int *bad = arg;
    while (!atomic_load_explicit(&sr.ready, memory_order_acquire))
        sched_yield();
    size_t i;
    while ((i = atomic_fetch_add(&sr.next, 1)) < PER_ROUND) {
        unsigned char *p = sr.blocks[i];
        if (p[0] != 0x5A || p[15] != 0x5A)
            atomic_fetch_add(bad, 1);
        ts_free(p);
    }
    return NULL;
}

TEST(stress_owner_exits_during_remote_frees) {
    const char *env = getenv("TS_TEST_SCALE");
    long rounds = 60 * (env && atol(env) > 0 ? atol(env) : 1);
    size_t before_live = abandoned_live();
    _Atomic int bad = 0;
    for (long r = 0; r < rounds; r++) {
        atomic_store(&sr.ready, 0);
        atomic_store(&sr.next, 0);
        pthread_t owner, f[FREERS];
        for (int i = 0; i < FREERS; i++)
            REQUIRE(pthread_create(&f[i], NULL, free_round, &bad) == 0);
        REQUIRE(pthread_create(&owner, NULL, owner_alloc_and_exit, NULL) == 0);
        pthread_join(owner, NULL);
        for (int i = 0; i < FREERS; i++)
            pthread_join(f[i], NULL);
    }
    CHECK_EQ(atomic_load(&bad), 0);
    /* Every block was freed, so nothing abandoned is live. */
    CHECK_EQ(abandoned_live(), before_live);
}
