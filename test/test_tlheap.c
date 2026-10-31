#include "test.h"

#include "heap.h"
#include "tsmalloc.h"

#include <pthread.h>
#include <sched.h>

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

static size_t used_pages(const ts_heap_t *heap) {
    size_t n = 0;
    for (const ts_segment_t *s = heap->segments; s != NULL; s = s->next)
        n += s->used_pages;
    return n;
}

static void release_all(ts_heap_t *heap) {
    while (heap->segments != NULL) {
        ts_segment_t *seg = heap->segments;
        heap->segments = seg->next;
        ts_segment_free(seg);
    }
    while (heap->seg_cache != NULL) {
        ts_segment_t *seg = heap->seg_cache;
        heap->seg_cache = seg->next;
        ts_segment_free(seg);
    }
}

static void free_remote(void *p) {
    ts_segment_t *seg = ts_ptr_segment(p);
    ts_heap_free_remote(seg, ts_ptr_page(seg, p), p);
}

typedef struct {
    size_t from, to;
} remote_job_t;

static void *free_remotely(void *arg) {
    remote_job_t *j = arg;
    for (size_t i = j->from; i < j->to; i++)
        free_remote(blocks[i]);
    return NULL;
}

TEST(remote_frees_are_reused_without_new_pages) {
    ts_heap_t heap;
    ts_heap_init(&heap);
    for (size_t i = 0; i < N; i++) {
        blocks[i] = ts_heap_malloc_small(&heap, 64);
        REQUIRE(blocks[i] != NULL);
    }
    size_t pages = used_pages(&heap);
    /* Two threads free the second half at once. */
    remote_job_t jobs[2] = {{N / 2, 3 * N / 4}, {3 * N / 4, N}};
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        REQUIRE(pthread_create(&t[i], NULL, free_remotely, &jobs[i]) == 0);
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);

    /* Nothing is back on a free list yet: the blocks sit on their pages'
     * remote lists or on the heap's delayed list. */
    CHECK_EQ(blocks_in_use(&heap), N);

    /* N more blocks: the free tail of the last page plus every block freed
     * above is just enough room, so a fresh page means the remote frees
     * weren't reused. */
    static void *more[N];
    for (size_t i = 0; i < N; i++) {
        more[i] = ts_heap_malloc_small(&heap, 64);
        REQUIRE(more[i] != NULL);
    }
    CHECK_EQ(used_pages(&heap), pages);
    /* And no block is handed out twice. */
    for (size_t i = 0; i < N / 2; i++)
        memset(blocks[i], 0x11, 64);
    for (size_t i = 0; i < N; i++)
        memset(more[i], (int)(i & 0x7F), 64);
    for (size_t i = 0; i < N / 2; i++)
        CHECK_EQ(((unsigned char *)blocks[i])[63], 0x11);
    for (size_t i = 0; i < N; i++)
        CHECK_EQ(((unsigned char *)more[i])[63], i & 0x7F);
    release_all(&heap);
}

static void *free_one(void *p) {
    free_remote(p);
    return NULL;
}

/* A page that left its queue full only hears about a remote free through
 * the heap's delayed list. */
TEST(remote_free_into_full_page_requeues_it) {
    ts_heap_t heap;
    ts_heap_init(&heap);
    unsigned cls = ts_size_to_class(1024);
    size_t n = 0;
    while (used_pages(&heap) < 3) {
        blocks[n] = ts_heap_malloc_small(&heap, 1024);
        REQUIRE(blocks[n] != NULL);
        n++;
    }
    void *victim = blocks[n - 2]; /* in the second page, now full */
    ts_page_t *page = ts_ptr_page(ts_ptr_segment(victim), victim);
    CHECK(!page->queued);
    CHECK_EQ(atomic_load(&page->thread_free), TS_TF_FULL);

    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, free_one, victim) == 0);
    pthread_join(t, NULL);
    CHECK(atomic_load(&heap.delayed_free) == victim);
    CHECK_EQ(atomic_load(&page->thread_free), 0);

    CHECK_EQ(ts_heap_collect_remote(&heap), 1);
    CHECK(page->queued);
    CHECK(heap.pages[cls].head == page);
    CHECK(ts_heap_malloc_small(&heap, 1024) == victim);
    release_all(&heap);
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

/* Stress: one owner keeps allocating while FREERS threads free its previous
 * batch, all into the same few pages at once. Batches alternate between two
 * buffers; the owner refills a buffer only after every freer is done with
 * it. TS_TEST_SCALE multiplies the rounds. */
enum { FREERS = 6, BATCH = 600 };

static struct {
    _Atomic long published; /* last round whose batch is ready */
    _Atomic long done[2];   /* freers finished with buffer i, summed over rounds */
    long rounds;
    unsigned char *batch[2][BATCH];
    _Atomic int errors;
} st;

static uint32_t tag_of(long round, size_t i) {
    return (uint32_t)(round * 2654435761u) ^ (uint32_t)i;
}

static void *stress_freer(void *arg) {
    size_t id = (size_t)(uintptr_t)arg;
    for (long r = 0; r < st.rounds; r++) {
        while (atomic_load_explicit(&st.published, memory_order_acquire) < r)
            sched_yield();
        for (size_t i = id; i < BATCH; i += FREERS) {
            unsigned char *p = st.batch[r & 1][i];
            uint32_t tag;
            memcpy(&tag, p, sizeof tag);
            if (tag != tag_of(r, i))
                atomic_fetch_add(&st.errors, 1);
            ts_free(p);
        }
        atomic_fetch_add_explicit(&st.done[r & 1], 1, memory_order_release);
    }
    return NULL;
}

TEST(stress_owner_allocates_while_others_free) {
    const char *env = getenv("TS_TEST_SCALE");
    st.rounds = 400 * (env && atol(env) > 0 ? atol(env) : 1);
    atomic_store(&st.published, -1);
    pthread_t t[FREERS];
    for (size_t i = 0; i < FREERS; i++)
        REQUIRE(pthread_create(&t[i], NULL, stress_freer, (void *)(uintptr_t)i) == 0);

    for (long r = 0; r < st.rounds; r++) {
        /* Buffer r & 1 was last used in round r - 2. */
        long need = (r / 2) * FREERS;
        while (atomic_load_explicit(&st.done[r & 1], memory_order_acquire) < need)
            sched_yield();
        for (size_t i = 0; i < BATCH; i++) {
            size_t size = 16 + (i % 8) * 16; /* a few classes, mostly shared pages */
            unsigned char *p = ts_malloc(size);
            REQUIRE(p != NULL);
            uint32_t tag = tag_of(r, i);
            memset(p, 0xAB, size);
            memcpy(p, &tag, sizeof tag);
            st.batch[r & 1][i] = p;
        }
        atomic_store_explicit(&st.published, r, memory_order_release);
    }
    for (size_t i = 0; i < FREERS; i++)
        pthread_join(t[i], NULL);
    CHECK_EQ(atomic_load(&st.errors), 0);

    /* Everything came back: the owner's pages are reusable, so a lot more
     * rounds didn't need more than a couple of segments. */
    void *p = ts_malloc(16);
    CHECK(count_segments(owner_of(p)) <= 2);
    ts_free(p);
}

/* Segments record the owning thread, which is what free uses to tell local
 * from remote. */
static void *report_thread_id(void *arg) {
    (void)arg;
    return (void *)ts_thread_id();
}

TEST(segment_records_owner_thread) {
    void *p = ts_malloc(48);
    REQUIRE(p != NULL);
    ts_segment_t *seg = ts_ptr_segment(p);
    CHECK_EQ(atomic_load(&seg->thread_id), ts_thread_id());
    CHECK_EQ(seg->heap->thread_id, ts_thread_id());

    pthread_t t;
    void *other;
    REQUIRE(pthread_create(&t, NULL, report_thread_id, NULL) == 0);
    pthread_join(t, &other);
    CHECK((uintptr_t)other != atomic_load(&seg->thread_id));
    ts_free(p);
}

/* A heap nobody owns (built by hand, as tests do) gets segments with owner
 * 0, so even its creator's ts_free treats the blocks as remote. */
TEST(unowned_heap_blocks_take_remote_path) {
    ts_heap_t heap;
    ts_heap_init(&heap);
    void *p = ts_heap_malloc_small(&heap, 64);
    REQUIRE(p != NULL);
    ts_segment_t *seg = ts_ptr_segment(p);
    ts_page_t *page = ts_ptr_page(seg, p);
    CHECK_EQ(atomic_load(&seg->thread_id), 0);
    ts_free(p);
    /* Still counted used: it waits on the page's remote list. */
    CHECK_EQ(page->used, 1);
    CHECK(atomic_load(&page->thread_free) == (uintptr_t)p);
    CHECK_EQ(ts_page_collect(page), 1);
    CHECK_EQ(page->used, 0);
    release_all(&heap);
}
