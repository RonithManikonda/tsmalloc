#include "test.h"

#include "tsmalloc.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <unistd.h>

/* fork from a process whose other threads are busy in every locked or
 * flag-guarded part of the allocator. Those threads don't exist in the child,
 * so anything they held at that moment must not stay held there.
 *
 * The child only frees blocks that no thread was touching when it forked.
 * fork doesn't copy memory at one instant: other threads keep running while
 * the kernel goes through the address space, so a shared slot can be copied
 * before a worker emptied it and the block after that worker freed it.
 * Freeing such a block in the child would be a double free in the test.
 *
 * A child that deadlocks is killed by its own alarm and fails the test. */

#if defined(__SANITIZE_THREAD__)
#define TS_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TS_UNDER_TSAN 1
#endif
#endif
#ifndef TS_UNDER_TSAN
#define TS_UNDER_TSAN 0
#endif

/* Linux ASan's allocator doesn't take its own locks around fork (checked
 * with gcc 13's libasan): a child thread can block forever on a size-class
 * lock some parent thread held at that moment. The hang is inside the
 * sanitizer runtime, not tsmalloc, so the child skips its thread there. */
#if defined(__linux__) && defined(__SANITIZE_ADDRESS__)
#define TS_UNDER_LINUX_ASAN 1
#elif defined(__linux__) && defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TS_UNDER_LINUX_ASAN 1
#endif
#endif
#ifndef TS_UNDER_LINUX_ASAN
#define TS_UNDER_LINUX_ASAN 0
#endif

enum {
    SLOTS = 4096,
    WORKERS = 4,
    INHERITED = 2000,
    FORKS = TS_UNDER_TSAN ? 15 : 60,
    CHILD_ALARM_SECS = 30,
};

static _Atomic(void *) slots[SLOTS];
static atomic_int stop;
/* Allocated by the workers before any fork and left alone until the end,
 * from the same pages as the blocks they churn through. */
static void *inherited[WORKERS][INHERITED];
static atomic_int workers_ready;

static size_t next_random(size_t *state) {
    *state = *state * 6364136223846793005u + 1442695040888963407u;
    return *state >> 33;
}

/* Mostly small blocks, swapped through shared slots so that nearly every
 * free is remote and pages keep going through the full/delayed path. Now
 * and then one big enough for the huge cache and its lock. */
static void *churn(void *arg) {
    size_t worker = (size_t)arg;
    size_t rng = worker * 2 + 1;
    for (size_t i = 0; i < INHERITED; i++)
        inherited[worker][i] = ts_malloc(64);
    atomic_fetch_add(&workers_ready, 1);
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        size_t r = next_random(&rng);
        size_t size = r % 64 == 0 ? (size_t)1 << 20 : 16 + r % 512;
        void *p = ts_malloc(size);
        void *old = atomic_exchange(&slots[r % SLOTS], p);
        ts_free(old);
    }
    return NULL;
}

static void *short_lived(void *arg) {
    /* Blocks left live in the slots keep this thread's segments from being
     * released, so its exit goes through the abandoned list. */
    size_t rng = (size_t)arg;
    for (int i = 0; i < 64; i++) {
        size_t r = next_random(&rng);
        ts_free(atomic_exchange(&slots[r % SLOTS], ts_malloc(32 + r % 200)));
    }
    return NULL;
}

/* Keeps the abandoned list busy. */
static void *thread_spawner(void *arg) {
    (void)arg;
    size_t n = 1;
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        pthread_t t;
        if (pthread_create(&t, NULL, short_lived, (void *)(n++ * 2 + 1)) == 0)
            pthread_join(t, NULL);
    }
    return NULL;
}

static void *child_thread(void *arg) {
    (void)arg;
    void *p = ts_malloc(100);
    if (p != NULL)
        memset(p, 3, 100);
    ts_free(p);
    return p;
}

/* In the child: only write(2)-safe failure reporting via the exit status. */
static void child_body(void) {
    alarm(CHILD_ALARM_SECS);

    /* Blocks from threads that don't exist here, freed into pages those
     * threads may have been in the middle of freeing into. */
    for (size_t w = 0; w < WORKERS; w++)
        for (size_t i = 0; i < INHERITED; i++)
            ts_free(inherited[w][i]);

    static const size_t sizes[] = {16, 1024, 20000, 600000, (size_t)1 << 20, (size_t)8 << 20};
    for (int round = 0; round < 4; round++) {
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            unsigned char *p = ts_malloc(sizes[i]);
            if (p == NULL)
                _exit(2);
            memset(p, 0xAB, sizes[i]);
            if (p[sizes[i] - 1] != 0xAB)
                _exit(3);
            ts_free(p);
        }
    }

    /* Enough small blocks to run into the slow path, which looks at the
     * abandoned list and the delayed list. */
    enum { N = 5000 };
    static void *blocks[N];
    for (int i = 0; i < N; i++)
        if ((blocks[i] = ts_malloc(64)) == NULL)
            _exit(4);
    for (int i = 0; i < N; i++)
        ts_free(blocks[i]);

/* TSan refuses to start threads after a multithreaded fork; see above for ASan. */
#if !TS_UNDER_TSAN && !TS_UNDER_LINUX_ASAN
    pthread_t t;
    if (pthread_create(&t, NULL, child_thread, NULL) != 0)
        _exit(5);
    pthread_join(t, NULL);
#else
    (void)child_thread;
#endif
    _exit(0);
}

TEST(fork_while_other_threads_allocate) {
    pthread_t workers[WORKERS], spawner;
    for (size_t i = 0; i < WORKERS; i++)
        REQUIRE(pthread_create(&workers[i], NULL, churn, (void *)i) == 0);
    while (atomic_load(&workers_ready) < WORKERS)
        sched_yield();
    REQUIRE(pthread_create(&spawner, NULL, thread_spawner, NULL) == 0);

    int bad = 0;
    for (int i = 0; i < FORKS && !bad; i++) {
        pid_t pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0)
            child_body();
        int status = 0;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "  fork %d: child status 0x%x (signal %d means it hung or crashed)\n", i,
                    status, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            bad = 1;
        }
    }
    CHECK(!bad);

    atomic_store(&stop, 1);
    for (size_t i = 0; i < WORKERS; i++)
        pthread_join(workers[i], NULL);
    pthread_join(spawner, NULL);
    for (size_t i = 0; i < SLOTS; i++)
        ts_free(atomic_exchange(&slots[i], NULL));
    for (size_t w = 0; w < WORKERS; w++)
        for (size_t i = 0; i < INHERITED; i++)
            ts_free(inherited[w][i]);
}

/* The parent has to come out of fork with everything working too. */
TEST(parent_keeps_working_after_fork) {
    void *before = ts_malloc((size_t)1 << 20);
    REQUIRE(before != NULL);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        alarm(CHILD_ALARM_SECS);
        ts_free(before);
        _exit(0);
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Still ours here, whatever the child did with its copy. */
    memset(before, 1, (size_t)1 << 20);
    ts_free(before);
    void *after = ts_malloc((size_t)1 << 20);
    CHECK(after != NULL);
    ts_free(after);
}
