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
    /* Our own heap first: setting it up may adopt, which would throw off
     * the counts below. */
    void *mine = ts_malloc(32);
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

/* ---- Adoption ---------------------------------------------------------- */

static struct {
    ts_segment_t *segs[64];
    size_t nsegs;
} left;

static void record_seg(ts_segment_t *seg, void *arg) {
    (void)arg;
    if (left.nsegs < 64)
        left.segs[left.nsegs++] = seg;
}

static int was_abandoned(const void *p) {
    for (size_t i = 0; i < left.nsegs; i++)
        if (ts_ptr_segment(p) == left.segs[i])
            return 1;
    return 0;
}

typedef struct {
    int reused;      /* got a block back from an abandoned segment */
    int owned;       /* and that segment now has our id */
    void *p[400];
} adopter_t;

static void *adopt_and_alloc(void *arg) {
    adopter_t *a = arg;
    for (size_t i = 0; i < 400; i++) {
        a->p[i] = ts_malloc(64);
        if (was_abandoned(a->p[i])) {
            a->reused = 1;
            a->owned |= atomic_load(&ts_ptr_segment(a->p[i])->thread_id) == ts_thread_id();
        }
    }
    for (size_t i = 0; i < 400; i++)
        ts_free(a->p[i]);
    return NULL;
}

TEST(new_thread_adopts_and_reuses_freed_blocks) {
    /* Make sure nothing is abandoned to start with, so the next thread's
     * leftovers are what gets adopted. */
    void *mine = ts_malloc(64);
    while (ts_abandoned_count() > 0)
        CHECK(ts_heap_adopt(ts_ptr_segment(mine)->heap));

    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, alloc_n, (void *)(uintptr_t)N) == 0);
    pthread_join(t, NULL);
    left.nsegs = 0;
    ts_abandoned_visit(record_seg, NULL);
    REQUIRE(left.nsegs > 0);

    /* Free most of it from here (remote: we don't own those segments). */
    for (size_t i = 0; i < N; i++)
        if (i % 8 != 0)
            ts_free(blocks[i]);

    static adopter_t a;
    REQUIRE(pthread_create(&t, NULL, adopt_and_alloc, &a) == 0);
    pthread_join(t, NULL);
    CHECK(a.reused);
    CHECK(a.owned);
    for (size_t i = 0; i < N; i += 8) {
        CHECK_EQ(((unsigned char *)blocks[i])[15], i & 0xFF);
        ts_free(blocks[i]);
    }
    ts_free(mine);
}

TEST(unowned_heap_never_adopts) {
    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, alloc_n, (void *)(uintptr_t)100) == 0);
    pthread_join(t, NULL);
    size_t n = ts_abandoned_count();
    REQUIRE(n > 0);
    ts_heap_t heap;
    ts_heap_init(&heap);
    CHECK(!ts_heap_adopt(&heap));
    CHECK_EQ(ts_abandoned_count(), n);
    for (size_t i = 0; i < 100; i++)
        ts_free(blocks[i]);
}

/* Churn of short-lived threads, each leaving blocks behind that are freed
 * after it is gone. Without adoption every thread would strand a segment;
 * with it the number of mapped segments stays flat. */
enum { CHURN_KEEP = 300 };
static void *kept[2][CHURN_KEEP];

static void *churn_thread(void *arg) {
    void **out = arg;
    void *mine[200];
    for (size_t i = 0; i < CHURN_KEEP; i++) {
        out[i] = ts_malloc(16 + (i * 40) % 2048);
        memset(out[i], 0x3C, 16);
    }
    for (size_t i = 0; i < 200; i++)
        mine[i] = ts_malloc(32 + i * 8);
    for (size_t i = 0; i < 200; i++)
        ts_free(mine[i]);
    return NULL;
}

TEST(churn_of_short_lived_threads_stays_bounded) {
    const char *env = getenv("TS_TEST_SCALE");
    long rounds = 300 * (env && atol(env) > 0 ? atol(env) : 1);
    size_t start = ts_segment_count();
    size_t peak = start;
    for (long r = 0; r < rounds; r++) {
        pthread_t t;
        REQUIRE(pthread_create(&t, NULL, churn_thread, kept[r & 1]) == 0);
        pthread_join(t, NULL);
        /* Free the previous thread's blocks, so each thread's leftovers
         * outlive it by one round. */
        if (r > 0)
            for (size_t i = 0; i < CHURN_KEEP; i++) {
                unsigned char *p = kept[(r - 1) & 1][i];
                CHECK_EQ(p[15], 0x3C);
                ts_free(p);
            }
        if (ts_segment_count() > peak)
            peak = ts_segment_count();
    }
    for (size_t i = 0; i < CHURN_KEEP; i++)
        ts_free(kept[(rounds - 1) & 1][i]);
    /* A couple of rounds' worth at most, not one per thread. */
    CHECK(peak - start <= 4);
    CHECK(ts_abandoned_count() <= 4);
}
