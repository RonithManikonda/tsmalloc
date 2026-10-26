/*
 * cache_scratch: passive false sharing (after Hoard's cache-scratch).
 *
 * The main thread allocates one small object per worker, back to back, so
 * they almost certainly share cache lines. Each worker frees the object it
 * was given, then repeatedly allocates an object of the same size, writes
 * to it REPS times and frees it. If the allocator hands the freed neighbour
 * memory back to a different thread, workers end up writing to the same
 * cache line and the line ping-pongs between cores. A per-thread allocator
 * keeps each worker on its own pages and the writes stay local.
 *
 * --iters is malloc/write/free rounds per thread; ops counts the writes.
 */
#include "bench.h"

enum { OBJ_SIZE = 8, REPS = 1000 };

typedef struct {
    const bench_alloc_t *alloc;
    char *given; /* allocated by the main thread */
    long rounds;
} worker_arg_t;

static void *worker(void *p) {
    worker_arg_t *a = p;
    a->alloc->free(a->given);
    for (long r = 0; r < a->rounds; r++) {
        /* volatile so every write really goes to memory and the loop
         * isn't folded into one store. */
        volatile char *obj = a->alloc->malloc(OBJ_SIZE);
        if (obj == NULL) {
            perror("cache_scratch: malloc");
            exit(1);
        }
        for (int i = 0; i < REPS; i++)
            for (int j = 0; j < OBJ_SIZE; j++)
                obj[j]++;
        a->alloc->free((void *)obj);
    }
    return NULL;
}

int main(int argc, char **argv) {
    bench_opts_t o = bench_parse(argc, argv, "cache_scratch", 20000);
    worker_arg_t *args = calloc((size_t)o.threads, sizeof *args);
    if (args == NULL) {
        perror("cache_scratch");
        return 1;
    }
    /* Allocated in a row by one thread: these are the neighbours. */
    for (int i = 0; i < o.threads; i++) {
        char *obj = o.alloc->malloc(OBJ_SIZE);
        if (obj == NULL) {
            perror("cache_scratch: malloc");
            return 1;
        }
        memset(obj, 0, OBJ_SIZE);
        args[i] = (worker_arg_t){o.alloc, obj, o.iters};
    }
    double secs = bench_run(o.threads, worker, args, sizeof args[0]);
    bench_report(&o, (double)o.threads * (double)o.iters * REPS * OBJ_SIZE, secs);
    free(args);
    return 0;
}
