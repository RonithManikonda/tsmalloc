/*
 * xmalloc: producer/consumer pairs. The producer allocates batches of
 * random-sized blocks (16..1024 B) and hands each full batch to its
 * consumer, which frees every block in it. All frees are remote: no block
 * is ever freed by the thread that allocated it.
 *
 * Threads run in pairs, so --threads is rounded up to an even number (and
 * the reported thread count is what actually ran). --iters is the number
 * of blocks each producer allocates.
 */
#include "bench.h"

#include <stdint.h>

enum { BATCH = 256, RING = 8, MIN_SIZE = 16, MAX_SIZE = 1024 };

/* The handoff ring lives in plain static-sized arrays, not in memory from
 * the allocator under test, and is touched once per batch, so its mutex
 * is a small fixed cost next to BATCH mallocs and frees. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
    void *slots[RING][BATCH];
    long head, tail; /* batches taken / batches put */
} ring_t;

typedef struct {
    const bench_alloc_t *alloc;
    ring_t *ring;
    long batches;
    int producer;
    uint64_t rng;
} worker_arg_t;

static void produce(worker_arg_t *a) {
    ring_t *q = a->ring;
    void *batch[BATCH];
    for (long n = 0; n < a->batches; n++) {
        for (int i = 0; i < BATCH; i++) {
            uint64_t x = a->rng;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            a->rng = x;
            size_t size = MIN_SIZE + (size_t)(x % (MAX_SIZE - MIN_SIZE + 1));
            char *b = a->alloc->malloc(size);
            if (b == NULL) {
                perror("xmalloc: malloc");
                exit(1);
            }
            b[0] = 1;
            batch[i] = b;
        }
        pthread_mutex_lock(&q->lock);
        while (q->tail - q->head == RING)
            pthread_cond_wait(&q->not_full, &q->lock);
        memcpy(q->slots[q->tail % RING], batch, sizeof batch);
        q->tail++;
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->lock);
    }
}

static void consume(worker_arg_t *a) {
    ring_t *q = a->ring;
    void *batch[BATCH];
    for (long n = 0; n < a->batches; n++) {
        pthread_mutex_lock(&q->lock);
        while (q->tail == q->head)
            pthread_cond_wait(&q->not_empty, &q->lock);
        memcpy(batch, q->slots[q->head % RING], sizeof batch);
        q->head++;
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->lock);
        /* Free outside the lock so the producer can keep going. */
        for (int i = 0; i < BATCH; i++)
            a->alloc->free(batch[i]);
    }
}

static void *worker(void *p) {
    worker_arg_t *a = p;
    if (a->producer)
        produce(a);
    else
        consume(a);
    return NULL;
}

int main(int argc, char **argv) {
    bench_opts_t o = bench_parse(argc, argv, "xmalloc", 2000000);
    int pairs = (o.threads + 1) / 2;
    o.threads = pairs * 2;
    long batches = (o.iters + BATCH - 1) / BATCH;

    ring_t *rings = calloc((size_t)pairs, sizeof *rings);
    worker_arg_t *args = calloc((size_t)o.threads, sizeof *args);
    if (rings == NULL || args == NULL) {
        perror("xmalloc");
        return 1;
    }
    for (int p = 0; p < pairs; p++) {
        pthread_mutex_init(&rings[p].lock, NULL);
        pthread_cond_init(&rings[p].not_empty, NULL);
        pthread_cond_init(&rings[p].not_full, NULL);
        for (int k = 0; k < 2; k++)
            args[2 * p + k] = (worker_arg_t){o.alloc, &rings[p], batches, k == 0,
                                             0x9e3779b97f4a7c15ull * (uint64_t)(p + 1)};
    }
    double secs = bench_run(o.threads, worker, args, sizeof args[0]);
    /* One op = one malloc on the producer plus its free on the consumer. */
    bench_report(&o, (double)pairs * (double)batches * BATCH, secs);

    for (int p = 0; p < pairs; p++) {
        pthread_mutex_destroy(&rings[p].lock);
        pthread_cond_destroy(&rings[p].not_empty);
        pthread_cond_destroy(&rings[p].not_full);
    }
    free(rings);
    free(args);
    return 0;
}
