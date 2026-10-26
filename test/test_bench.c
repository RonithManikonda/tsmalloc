#include "test.h"

#include "../bench/bench.h"

#include <stdatomic.h>

/* The benchmark harness is shared by every bench program, so check its
 * flag parsing and thread runner once here. */

TEST(parse_defaults) {
    char *argv[] = {"b", NULL};
    bench_opts_t o = bench_parse(1, argv, "b", 123);
    CHECK(o.alloc == &bench_ts);
    CHECK_EQ(o.threads, 1);
    CHECK_EQ(o.iters, 123);
}

TEST(parse_flags) {
    char *argv[] = {"b", "--alloc=system", "--threads=6", "--iters=5000", NULL};
    bench_opts_t o = bench_parse(4, argv, "b", 1);
    CHECK(o.alloc == &bench_system);
    CHECK_EQ(o.threads, 6);
    CHECK_EQ(o.iters, 5000);

    char *argv2[] = {"b", "--alloc=ts", NULL};
    CHECK(bench_parse(2, argv2, "b", 1).alloc == &bench_ts);
}

TEST(flag_needs_exact_key) {
    CHECK(bench_flag("--threads=3", "threads") != NULL);
    CHECK(bench_flag("--threadsx=3", "threads") == NULL);
    CHECK(bench_flag("-threads=3", "threads") == NULL);
    CHECK(bench_flag("--threads", "threads") == NULL);
}

TEST(both_allocators_work_through_dispatch) {
    const bench_alloc_t *allocs[] = {&bench_system, &bench_ts};
    for (int i = 0; i < 2; i++) {
        char *p = allocs[i]->malloc(100);
        REQUIRE(p != NULL);
        memset(p, 1, 100);
        p = allocs[i]->realloc(p, 5000);
        REQUIRE(p != NULL);
        CHECK_EQ(p[99], 1);
        allocs[i]->free(p);
    }
}

typedef struct {
    atomic_int *running;
    int max_seen;
    long done;
} gate_arg_t;

static void *gate_worker(void *p) {
    gate_arg_t *a = p;
    /* Everyone is released together, so the last thread in sees them all. */
    int now = atomic_fetch_add(a->running, 1) + 1;
    a->max_seen = now;
    for (volatile long i = 0; i < 100000; i++)
        ;
    a->done = 1;
    return NULL;
}

TEST(run_starts_all_threads_and_waits) {
    enum { N = 6 };
    atomic_int running = 0;
    gate_arg_t args[N];
    for (int i = 0; i < N; i++)
        args[i] = (gate_arg_t){&running, 0, 0};
    double secs = bench_run(N, gate_worker, args, sizeof args[0]);
    CHECK(secs >= 0);
    CHECK_EQ(atomic_load(&running), N);
    int max = 0;
    for (int i = 0; i < N; i++) {
        CHECK_EQ(args[i].done, 1);
        if (args[i].max_seen > max)
            max = args[i].max_seen;
    }
    CHECK_EQ(max, N);
}
