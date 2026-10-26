/*
 * Shared harness for the benchmark programs.
 *
 *   bench_opts_t o = bench_parse(argc, argv, "threadtest", 1000000);
 *   double secs = bench_run(o.threads, worker, args, sizeof args[0]);
 *   bench_report(&o, total_ops, secs);
 *
 * Every program takes the same flags:
 *   --alloc=system|ts   which allocator the workers call (default ts)
 *   --threads=N         worker threads (default 1)
 *   --iters=N           work per thread, meaning is up to the program
 * and prints one line of key=value pairs, so scripts can grep and the
 * output still reads fine by eye.
 *
 * The allocator is picked at run time through function pointers, so both
 * sides pay the same indirect call and the same binary measures both.
 */
#ifndef TS_BENCH_H
#define TS_BENCH_H

/* Users are compiled with _POSIX_C_SOURCE=200809L (bench/CMakeLists.txt)
 * so clock_gettime is visible under -std=c11 on glibc. */
#include "tsmalloc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *name;
    void *(*malloc)(size_t);
    void (*free)(void *);
    void *(*realloc)(void *, size_t);
} bench_alloc_t;

static const bench_alloc_t bench_system = {"system", malloc, free, realloc};
static const bench_alloc_t bench_ts = {"ts", ts_malloc, ts_free, ts_realloc};

typedef struct {
    const char *bench;
    const bench_alloc_t *alloc;
    int threads;
    long iters;
} bench_opts_t;

static inline void bench_usage(const char *bench) {
    fprintf(stderr, "usage: %s [--alloc=system|ts] [--threads=N] [--iters=N]\n", bench);
    exit(2);
}

/* Value of `--key=` in arg, or NULL. */
static inline const char *bench_flag(const char *arg, const char *key) {
    size_t n = strlen(key);
    if (strncmp(arg, "--", 2) != 0 || strncmp(arg + 2, key, n) != 0 || arg[2 + n] != '=')
        return NULL;
    return arg + 3 + n;
}

static inline long bench_positive(const char *bench, const char *v) {
    char *end;
    long n = strtol(v, &end, 10);
    if (*v == '\0' || *end != '\0' || n <= 0)
        bench_usage(bench);
    return n;
}

/* Parses the common flags; `default_iters` applies when --iters is absent. */
static inline bench_opts_t bench_parse(int argc, char **argv, const char *bench, long default_iters) {
    bench_opts_t o = {bench, &bench_ts, 1, default_iters};
    for (int i = 1; i < argc; i++) {
        const char *v;
        if ((v = bench_flag(argv[i], "alloc")) != NULL) {
            if (strcmp(v, "system") == 0)
                o.alloc = &bench_system;
            else if (strcmp(v, "ts") == 0)
                o.alloc = &bench_ts;
            else
                bench_usage(bench);
        } else if ((v = bench_flag(argv[i], "threads")) != NULL) {
            o.threads = (int)bench_positive(bench, v);
        } else if ((v = bench_flag(argv[i], "iters")) != NULL) {
            o.iters = bench_positive(bench, v);
        } else {
            bench_usage(bench);
        }
    }
    return o;
}

static inline double bench_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* macOS has no pthread_barrier_t, so the start gate is a counter and a
 * condition variable. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int waiting, total;
    double start;
} bench_gate_t;

typedef struct {
    bench_gate_t *gate;
    void *(*fn)(void *);
    void *arg;
} bench_thread_t;

static inline void *bench_trampoline(void *p) {
    bench_thread_t *t = p;
    bench_gate_t *g = t->gate;
    pthread_mutex_lock(&g->lock);
    /* The last thread to arrive starts the clock and opens the gate, so
     * thread creation isn't part of the measurement. */
    if (++g->waiting == g->total) {
        g->start = bench_now();
        pthread_cond_broadcast(&g->cond);
    } else {
        while (g->waiting < g->total)
            pthread_cond_wait(&g->cond, &g->lock);
    }
    pthread_mutex_unlock(&g->lock);
    return t->fn(t->arg);
}

/* Run fn on `threads` threads, the i-th getting (char *)args + i * stride,
 * all released at once. Returns seconds from release until the last one
 * finished. */
static inline double bench_run(int threads, void *(*fn)(void *), void *args, size_t stride) {
    bench_gate_t gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, threads, 0};
    pthread_t *th = calloc((size_t)threads, sizeof *th);
    bench_thread_t *slots = calloc((size_t)threads, sizeof *slots);
    if (th == NULL || slots == NULL) {
        perror("bench_run");
        exit(1);
    }
    for (int i = 0; i < threads; i++) {
        slots[i] = (bench_thread_t){&gate, fn, (char *)args + (size_t)i * stride};
        if (pthread_create(&th[i], NULL, bench_trampoline, &slots[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }
    for (int i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    double secs = bench_now() - gate.start;
    free(th);
    free(slots);
    return secs;
}

static inline void bench_report(const bench_opts_t *o, double ops, double secs) {
    printf("bench=%s alloc=%s threads=%d ops=%.0f secs=%.3f mops=%.2f\n",
           o->bench, o->alloc->name, o->threads, ops, secs,
           secs > 0 ? ops / secs / 1e6 : 0.0);
}

#endif /* TS_BENCH_H */
