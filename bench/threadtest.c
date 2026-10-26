/*
 * threadtest: every thread allocates a batch of same-sized blocks, then
 * frees them all, over and over. Nothing crosses threads, so this is the
 * best case for any allocator with per-thread caches and the worst case
 * for one behind a single lock: every call contends on it.
 *
 * Runs once per size (16, 64, 256, 1024 B) and prints one line each.
 * --iters is the number of batches per thread.
 */
#include "bench.h"

enum { BATCH = 100 };

static const size_t sizes[] = {16, 64, 256, 1024};

typedef struct {
    const bench_alloc_t *alloc;
    size_t size;
    long rounds;
} worker_arg_t;

static void *worker(void *p) {
    worker_arg_t *a = p;
    void *blocks[BATCH];
    for (long r = 0; r < a->rounds; r++) {
        for (int i = 0; i < BATCH; i++) {
            char *b = a->alloc->malloc(a->size);
            if (b == NULL) {
                perror("threadtest: malloc");
                exit(1);
            }
            /* Touch the block so a lazily built free list or untouched
             * mmap page isn't measured as free. */
            b[0] = (char)i;
            blocks[i] = b;
        }
        for (int i = 0; i < BATCH; i++)
            a->alloc->free(blocks[i]);
    }
    return NULL;
}

int main(int argc, char **argv) {
    bench_opts_t o = bench_parse(argc, argv, "threadtest", 20000);
    worker_arg_t *args = calloc((size_t)o.threads, sizeof *args);
    if (args == NULL) {
        perror("threadtest");
        return 1;
    }
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        for (int i = 0; i < o.threads; i++)
            args[i] = (worker_arg_t){o.alloc, sizes[s], o.iters};
        double secs = bench_run(o.threads, worker, args, sizeof args[0]);

        char name[32];
        snprintf(name, sizeof name, "threadtest-%zu", sizes[s]);
        bench_opts_t line = o;
        line.bench = name;
        /* One op = one malloc plus its free. */
        bench_report(&line, (double)o.threads * (double)o.iters * BATCH, secs);
    }
    free(args);
    return 0;
}
