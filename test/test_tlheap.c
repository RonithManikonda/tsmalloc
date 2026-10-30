#include "test.h"

#include "heap.h"
#include "tsmalloc.h"

#include <pthread.h>

/* Thread-local heaps: who owns a block, and how blocks freed by another
 * thread get back to their owner. */

static ts_heap_t *owner_of(const void *p) {
    return ts_ptr_segment(p)->heap;
}

static size_t count_segments(const ts_heap_t *heap) {
    size_t n = 0;
    for (const ts_segment_t *s = heap->segments; s != NULL; s = s->next)
        n++;
    return n;
}

static size_t blocks_in_use(const ts_heap_t *heap) {
    size_t n = 0;
    for (const ts_segment_t *s = heap->segments; s != NULL; s = s->next)
        for (size_t i = 0; i < TS_PAGES_PER_SEGMENT; i++)
            if (s->pages[i].in_use)
                n += s->pages[i].used;
    return n;
}

static void *malloc_and_report_heap(void *arg) {
    (void)arg;
    void *p = ts_malloc(64);
    ts_heap_t *h = p ? owner_of(p) : NULL;
    ts_free(p);
    return h;
}

TEST(each_thread_gets_its_own_heap) {
    void *p = ts_malloc(64);
    REQUIRE(p != NULL);
    pthread_t t;
    void *theirs;
    REQUIRE(pthread_create(&t, NULL, malloc_and_report_heap, NULL) == 0);
    pthread_join(t, &theirs);
    CHECK(theirs != NULL);
    CHECK(theirs != owner_of(p));
    /* And the same thread keeps its heap. */
    void *q = ts_malloc(4000);
    CHECK(owner_of(q) == owner_of(p));
    ts_free(p);
    ts_free(q);
}

enum { N = 2000 };
static void *blocks[N];

typedef struct {
    ts_heap_t *heap;
    size_t from, to;
} remote_job_t;

static void *free_remotely(void *arg) {
    remote_job_t *j = arg;
    for (size_t i = j->from; i < j->to; i++)
        ts_heap_free_remote(j->heap, blocks[i]);
    return NULL;
}

TEST(remote_frees_wait_for_the_owner) {
    ts_heap_t heap;
    ts_heap_init(&heap);
    for (size_t i = 0; i < N; i++) {
        blocks[i] = ts_heap_malloc_small(&heap, 64);
        REQUIRE(blocks[i] != NULL);
    }
    /* Two threads free the second half at once. */
    remote_job_t jobs[2] = {{&heap, N / 2, 3 * N / 4}, {&heap, 3 * N / 4, N}};
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        REQUIRE(pthread_create(&t[i], NULL, free_remotely, &jobs[i]) == 0);
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);

    /* Nothing went back to a page yet: the blocks are only queued. */
    CHECK_EQ(blocks_in_use(&heap), N);

    CHECK_EQ(ts_heap_collect_remote(&heap), N / 2);
    CHECK_EQ(blocks_in_use(&heap), N / 2);
    CHECK_EQ(ts_heap_collect_remote(&heap), 0);
    /* The next block comes from the ones just returned. */
    void *again = ts_heap_malloc_small(&heap, 64);
    int reused = 0;
    for (size_t i = N / 2; i < N; i++)
        reused |= again == blocks[i];
    CHECK(reused);

    while (heap.segments != NULL) {
        ts_segment_t *seg = heap.segments;
        heap.segments = seg->next;
        ts_segment_free(seg);
    }
}

static void *free_all(void *arg) {
    size_t n = *(size_t *)arg;
    for (size_t i = 0; i < n; i++)
        ts_free(blocks[i]);
    return NULL;
}

/* A thread whose blocks are always freed by someone else must still reuse
 * its pages rather than grow a segment per round. */
TEST(producer_reuses_pages_freed_elsewhere) {
    size_t n = N;
    ts_heap_t *mine = NULL;
    size_t peak = 0;
    for (int round = 0; round < 100; round++) {
        for (size_t i = 0; i < n; i++) {
            blocks[i] = ts_malloc(1024);
            REQUIRE(blocks[i] != NULL);
            memset(blocks[i], round, 1024);
        }
        mine = owner_of(blocks[0]);
        size_t segs = count_segments(mine);
        if (segs > peak)
            peak = segs;
        pthread_t t;
        REQUIRE(pthread_create(&t, NULL, free_all, &n) == 0);
        pthread_join(t, NULL);
    }
    /* 2 MiB live per round, 200 MiB over the test: one segment holds a
     * round, and the previous round's blocks only come back once the
     * first fresh page is needed, so at most two are ever live. */
    CHECK(peak <= 2);
}
