#include "test.h"

#include "tsmalloc.h"

#include <pthread.h>

/* Smoke tests for the thread-local heaps: many threads allocating, writing,
 * verifying and freeing at once, plus blocks freed by a different thread
 * than the one that allocated them. TS_TEST_SCALE multiplies the work. */

enum { THREADS = 8, SLOTS = 256 };

static long scale(void) {
    const char *s = getenv("TS_TEST_SCALE");
    long v = s ? atol(s) : 1;
    return v > 0 ? v : 1;
}

/* xorshift: cheap, thread-private, good enough to pick sizes. */
static uint32_t rnd(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static size_t pick_size(uint32_t *rs) {
    uint32_t r = rnd(rs);
    if ((r & 63) == 0)
        return 16384 + (r >> 8) % 100000; /* the occasional large block */
    return 1 + (r >> 8) % ((r & 1) ? 128 : 4096);
}

static void stamp(unsigned char *p, size_t n, uint32_t tag) {
    memset(p, (int)(tag & 0xFF), n);
    if (n >= sizeof tag)
        memcpy(p, &tag, sizeof tag);
}

static int stamp_ok(const unsigned char *p, size_t n, uint32_t tag) {
    if (n >= sizeof tag) {
        uint32_t got;
        memcpy(&got, p, sizeof got);
        if (got != tag)
            return 0;
    }
    for (size_t i = sizeof tag; i < n; i++)
        if (p[i] != (unsigned char)(tag & 0xFF))
            return 0;
    return 1;
}

typedef struct {
    uint32_t id;
    int errors;
} worker_t;

static void *churn(void *arg) {
    worker_t *w = arg;
    unsigned char *ptr[SLOTS] = {0};
    size_t len[SLOTS] = {0};
    uint32_t tag[SLOTS] = {0};
    uint32_t rs = 0x9E3779B9u * (w->id + 1);
    long iters = 40000 * scale();

    for (long i = 0; i < iters; i++) {
        unsigned s = rnd(&rs) % SLOTS;
        if (ptr[s] != NULL) {
            if (!stamp_ok(ptr[s], len[s], tag[s]))
                w->errors++;
            if (rnd(&rs) & 1) {
                ts_free(ptr[s]);
                ptr[s] = NULL;
                continue;
            }
            size_t n = pick_size(&rs);
            unsigned char *q = ts_realloc(ptr[s], n);
            if (q == NULL) {
                w->errors++;
                continue;
            }
            size_t kept = len[s] < n ? len[s] : n;
            if (!stamp_ok(q, kept, tag[s]))
                w->errors++;
            ptr[s] = q;
            len[s] = n;
        } else {
            len[s] = pick_size(&rs);
            ptr[s] = ts_malloc(len[s]);
            if (ptr[s] == NULL) {
                w->errors++;
                continue;
            }
        }
        tag[s] = (w->id << 24) ^ (uint32_t)i;
        stamp(ptr[s], len[s], tag[s]);
    }
    for (unsigned s = 0; s < SLOTS; s++)
        ts_free(ptr[s]);
    return NULL;
}

TEST(threads_churn_independently) {
    pthread_t th[THREADS];
    worker_t w[THREADS];
    for (uint32_t i = 0; i < THREADS; i++) {
        w[i] = (worker_t){.id = i};
        REQUIRE(pthread_create(&th[i], NULL, churn, &w[i]) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        CHECK_EQ(w[i].errors, 0);
    }
}

/* Producers allocate and stamp blocks, consumers check and free them. The
 * handoff is a mutex-protected ring, so every block is freed by a thread
 * other than its allocator. */
enum { RING = 1024, PRODUCERS = 4, CONSUMERS = 4 };

static struct {
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
    struct { unsigned char *p; size_t n; uint32_t tag; } items[RING];
    size_t head, tail;
    int producers_left;
} ring = {.lock = PTHREAD_MUTEX_INITIALIZER,
          .not_empty = PTHREAD_COND_INITIALIZER,
          .not_full = PTHREAD_COND_INITIALIZER};

static void *produce(void *arg) {
    worker_t *w = arg;
    uint32_t rs = 0x85EBCA6Bu * (w->id + 1);
    long count = 20000 * scale();
    for (long i = 0; i < count; i++) {
        size_t n = pick_size(&rs);
        unsigned char *p = ts_malloc(n);
        if (p == NULL) {
            w->errors++;
            continue;
        }
        uint32_t tag = (w->id << 24) ^ (uint32_t)i;
        stamp(p, n, tag);
        pthread_mutex_lock(&ring.lock);
        while (ring.tail - ring.head == RING)
            pthread_cond_wait(&ring.not_full, &ring.lock);
        ring.items[ring.tail % RING].p = p;
        ring.items[ring.tail % RING].n = n;
        ring.items[ring.tail % RING].tag = tag;
        ring.tail++;
        pthread_cond_signal(&ring.not_empty);
        pthread_mutex_unlock(&ring.lock);
    }
    pthread_mutex_lock(&ring.lock);
    ring.producers_left--;
    pthread_cond_broadcast(&ring.not_empty);
    pthread_mutex_unlock(&ring.lock);
    return NULL;
}

static void *consume(void *arg) {
    worker_t *w = arg;
    for (;;) {
        pthread_mutex_lock(&ring.lock);
        while (ring.tail == ring.head && ring.producers_left > 0)
            pthread_cond_wait(&ring.not_empty, &ring.lock);
        if (ring.tail == ring.head) {
            pthread_mutex_unlock(&ring.lock);
            return NULL;
        }
        unsigned char *p = ring.items[ring.head % RING].p;
        size_t n = ring.items[ring.head % RING].n;
        uint32_t tag = ring.items[ring.head % RING].tag;
        ring.head++;
        pthread_cond_signal(&ring.not_full);
        pthread_mutex_unlock(&ring.lock);

        if (!stamp_ok(p, n, tag))
            w->errors++;
        ts_free(p);
    }
}

TEST(blocks_freed_by_other_threads) {
    pthread_t th[PRODUCERS + CONSUMERS];
    worker_t w[PRODUCERS + CONSUMERS];
    ring.producers_left = PRODUCERS;
    for (uint32_t i = 0; i < PRODUCERS + CONSUMERS; i++) {
        w[i] = (worker_t){.id = i};
        REQUIRE(pthread_create(&th[i], NULL, i < PRODUCERS ? produce : consume, &w[i]) == 0);
    }
    for (int i = 0; i < PRODUCERS + CONSUMERS; i++) {
        pthread_join(th[i], NULL);
        CHECK_EQ(w[i].errors, 0);
    }
    CHECK_EQ(ring.head, ring.tail);
}
