/*
 * larson: a simulation of a server where worker threads come and go
 * (after Larson & Krishnan's benchmark, as used by Hoard and mimalloc).
 *
 * Each thread owns an array of SLOTS live blocks. A "round" picks a random
 * slot, frees its block and puts a new random-sized one (16..1024 B) in its
 * place. After its share of rounds the thread hands the whole array to a
 * freshly created thread and exits, so the next generation frees blocks
 * that a different thread allocated: cross-thread frees, the case that
 * makes per-thread allocators work hard.
 *
 * --iters is the number of rounds per thread, spread over GENERATIONS.
 */
#include "bench.h"

#include <stdint.h>

enum { SLOTS = 1000, GENERATIONS = 8, MIN_SIZE = 16, MAX_SIZE = 1024 };

typedef struct {
    const bench_alloc_t *alloc;
    long rounds;      /* rounds for the current generation */
    uint64_t rng;
    unsigned char *blocks[SLOTS];
    unsigned char tags[SLOTS];
} lineage_t;

static uint64_t next_rand(uint64_t *s) {
    /* xorshift64: cheap, and good enough to scatter slots and sizes. */
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static unsigned char *new_block(lineage_t *l, int slot) {
    size_t size = MIN_SIZE + (size_t)(next_rand(&l->rng) % (MAX_SIZE - MIN_SIZE + 1));
    unsigned char *b = l->alloc->malloc(size);
    if (b == NULL) {
        perror("larson: malloc");
        exit(1);
    }
    /* Tag the block so a later free can check nobody else wrote to it. */
    unsigned char tag = (unsigned char)next_rand(&l->rng);
    b[0] = tag;
    b[size - 1] = tag;
    l->tags[slot] = tag;
    return b;
}

static void drop_block(lineage_t *l, int slot) {
    if (l->blocks[slot][0] != l->tags[slot]) {
        fprintf(stderr, "larson: block in slot %d was overwritten\n", slot);
        abort();
    }
    l->alloc->free(l->blocks[slot]);
}

/* One generation: replace random blocks, most of them allocated by an
 * earlier thread. */
static void *generation(void *p) {
    lineage_t *l = p;
    for (long r = 0; r < l->rounds; r++) {
        int slot = (int)(next_rand(&l->rng) % SLOTS);
        drop_block(l, slot);
        l->blocks[slot] = new_block(l, slot);
    }
    return NULL;
}

static void *lineage(void *p) {
    lineage_t *l = p;
    for (int i = 0; i < SLOTS; i++)
        l->blocks[i] = new_block(l, i);
    long left = l->rounds;
    for (int g = 0; g < GENERATIONS; g++) {
        l->rounds = left / (GENERATIONS - g);
        left -= l->rounds;
        pthread_t t;
        if (pthread_create(&t, NULL, generation, l) != 0) {
            perror("pthread_create");
            exit(1);
        }
        /* The join orders the child's writes to l before the next one
         * reads it, so the array needs no locking. */
        pthread_join(t, NULL);
    }
    for (int i = 0; i < SLOTS; i++)
        drop_block(l, i);
    return NULL;
}

int main(int argc, char **argv) {
    bench_opts_t o = bench_parse(argc, argv, "larson", 2000000);
    lineage_t *ls = calloc((size_t)o.threads, sizeof *ls);
    if (ls == NULL) {
        perror("larson");
        return 1;
    }
    for (int i = 0; i < o.threads; i++) {
        ls[i].alloc = o.alloc;
        ls[i].rounds = o.iters;
        ls[i].rng = 0x9e3779b97f4a7c15ull * (uint64_t)(i + 1);
    }
    double secs = bench_run(o.threads, lineage, ls, sizeof ls[0]);
    /* One op = one free plus one malloc; the initial fill and final drain
     * are timed but not counted. */
    bench_report(&o, (double)o.threads * (double)o.iters, secs);
    free(ls);
    return 0;
}
