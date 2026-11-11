#include "test.h"

#include "tsmalloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

/* Seeded randomized fuzz test over every public call.
 *
 * Each run keeps a table of live blocks, and for every block a shadow copy
 * of what its bytes should be, held in the system allocator. Random
 * operations allocate, write, realloc, check and free blocks; after every
 * operation that should leave a block's contents alone, the block is
 * compared with its shadow. Sizes go up to 1 MiB, so the small, medium,
 * large and huge paths all get hit, and so do realloc moves between them.
 *
 * The seed comes from TS_FUZZ_SEED if set (decimal or 0x hex), otherwise
 * from the clock. It is printed at the start and again on any failure, so
 * a failing run can be repeated exactly:
 *
 *   TS_FUZZ_SEED=0x1234abcd ./test_fuzz single
 *
 * The threaded test runs one independent fuzzer per thread (seed + thread
 * index), so each thread's sequence is reproducible even though their
 * interleaving isn't: a thread's failure names that thread's own seed,
 * which `single` replays on its own. TS_TEST_SCALE multiplies the
 * operation count. */

enum { SLOTS = 384, OPS = 60000, THREADS = 4 };

typedef struct {
    unsigned char *p;
    unsigned char *shadow; /* system malloc; NULL when size is 0 */
    size_t size;           /* bytes we may use and have written */
    size_t align;          /* alignment it was requested with, or 0 */
} slot_t;

typedef struct {
    uint64_t seed;
    uint64_t state;
    long op;
    const char *what; /* name of the operation in progress */
    slot_t slots[SLOTS];
    int failed;
} fuzz_t;

/* The calls whose bad arguments we test go through volatile pointers, or
 * the compiler folds impossible requests to NULL without setting errno. */
static void *(*volatile calloc_fn)(size_t, size_t) = ts_calloc;
static void *(*volatile realloc_fn)(void *, size_t) = ts_realloc;
static int (*volatile memalign_fn)(void **, size_t, size_t) = ts_posix_memalign;

static long scale(void) {
    const char *s = getenv("TS_TEST_SCALE");
    long v = s ? atol(s) : 1;
    return v > 0 ? v : 1;
}

static uint64_t base_seed(void) {
    static uint64_t seed;
    if (!seed) {
        const char *s = getenv("TS_FUZZ_SEED");
        if (s && *s) {
            seed = strtoull(s, NULL, 0);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            seed = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^
                   ((uint64_t)getpid() << 40);
        }
        if (!seed)
            seed = 1; /* xorshift's one bad state */
        printf("  fuzz seed 0x%" PRIx64 " (set TS_FUZZ_SEED to repeat)\n", seed);
    }
    return seed;
}

static uint64_t rnd(fuzz_t *f) {
    uint64_t x = f->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return f->state = x;
}

static uint64_t below(fuzz_t *f, uint64_t n) { return rnd(f) % n; }

/* Reports the first failure of a run with everything needed to replay it,
 * then makes the run stop: later checks would only report the fallout. */
#define FUZZ_CHECK(f, cond)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr,                                                   \
                    "  %s:%d: fuzz check (%s) failed in %s at op %ld, "       \
                    "seed 0x%" PRIx64 "\n",                                   \
                    __FILE__, __LINE__, #cond, (f)->what, (f)->op,            \
                    (f)->seed);                                               \
            (f)->failed = 1;                                                  \
            return;                                                           \
        }                                                                     \
    } while (0)

/* Mostly small, a steady share of medium sizes, and about one in sixteen
 * between 16 KiB and 1 MiB. Within each band the size is log-uniform so
 * the small classes near each power of two all come up. */
static size_t pick_size(fuzz_t *f) {
    uint64_t r = below(f, 16);
    unsigned max_shift = r == 0 ? 20 : r < 4 ? 14 : 10;
    unsigned shift = (unsigned)below(f, max_shift + 1);
    size_t lo = shift ? (size_t)1 << (shift - 1) : 0;
    return lo + (size_t)below(f, ((size_t)1 << shift) - lo + 1);
}

static size_t pick_align(fuzz_t *f) {
    /* 8 B .. 1 MiB, weighted toward the small end. */
    unsigned shift = 3 + (unsigned)(below(f, 4) ? below(f, 6) : below(f, 18));
    return (size_t)1 << shift;
}

/* Random bytes, written 8 at a time where possible. */
static void random_bytes(fuzz_t *f, unsigned char *dst, size_t n) {
    while (n >= 8) {
        uint64_t v = rnd(f);
        memcpy(dst, &v, 8);
        dst += 8;
        n -= 8;
    }
    if (n) {
        uint64_t v = rnd(f);
        memcpy(dst, &v, n);
    }
}

static void slot_check(fuzz_t *f, slot_t *s) {
    if (!s->p)
        return;
    FUZZ_CHECK(f, ts_usable_size(s->p) >= s->size);
    if (s->align)
        FUZZ_CHECK(f, ((uintptr_t)s->p & (s->align - 1)) == 0);
    FUZZ_CHECK(f, s->size == 0 || memcmp(s->p, s->shadow, s->size) == 0);
}

/* Takes ownership of a fresh block `p` whose first `zeroed` bytes are
 * known to be zero (calloc), fills the rest, and records it. Sometimes the
 * slot claims all of ts_usable_size: that space is the caller's to use, so
 * writing it must not corrupt anything. */
static void slot_set(fuzz_t *f, slot_t *s, void *p, size_t size, size_t align,
                     size_t zeroed) {
    FUZZ_CHECK(f, p != NULL);
    if (align)
        FUZZ_CHECK(f, ((uintptr_t)p & (align - 1)) == 0);
    else
        FUZZ_CHECK(f, ((uintptr_t)p & 15) == 0);
    size_t usable = ts_usable_size(p);
    FUZZ_CHECK(f, usable >= size);
    if (below(f, 8) == 0 && usable <= ((size_t)2 << 20))
        size = usable;

    unsigned char *shadow = size ? malloc(size) : NULL;
    FUZZ_CHECK(f, size == 0 || shadow != NULL);
    if (zeroed) {
        for (size_t i = 0; i < zeroed; i++)
            FUZZ_CHECK(f, ((unsigned char *)p)[i] == 0);
        memset(shadow, 0, zeroed);
    }
    random_bytes(f, shadow + zeroed, size - zeroed);
    if (size)
        memcpy(p, shadow, size);
    *s = (slot_t){.p = p, .shadow = shadow, .size = size, .align = align};
}

static void slot_free(fuzz_t *f, slot_t *s) {
    slot_check(f, s);
    ts_free(s->p);
    free(s->shadow);
    *s = (slot_t){0};
}

static void op_alloc(fuzz_t *f, slot_t *s) {
    size_t size = pick_size(f);
    switch (below(f, 4)) {
    case 0:
        f->what = "malloc";
        slot_set(f, s, ts_malloc(size), size, 0, 0);
        break;
    case 1: {
        f->what = "calloc";
        size_t count = 1 + (size_t)below(f, 16);
        size_t each = size / count;
        slot_set(f, s, ts_calloc(count, each), count * each, 0, count * each);
        break;
    }
    case 2: {
        f->what = "aligned_alloc";
        size_t align = pick_align(f);
        slot_set(f, s, ts_aligned_alloc(align, size), size, align, 0);
        break;
    }
    default: {
        f->what = "posix_memalign";
        size_t align = pick_align(f);
        void *p = NULL;
        FUZZ_CHECK(f, ts_posix_memalign(&p, align, size) == 0);
        slot_set(f, s, p, size, align, 0);
    }
    }
}

static void op_realloc(fuzz_t *f, slot_t *s) {
    f->what = "realloc";
    slot_check(f, s);
    if (f->failed)
        return;
    size_t size;
    switch (below(f, 8)) {
    case 0: size = 0; break;                                      /* minimal block */
    case 1: size = s->size + (size_t)below(f, 64); break;         /* nudge up */
    case 2: size = s->size - (size_t)below(f, s->size + 1); break; /* shrink */
    default: size = pick_size(f);
    }
    unsigned char *q = ts_realloc(s->p, size);
    FUZZ_CHECK(f, q != NULL);
    FUZZ_CHECK(f, ((uintptr_t)q & 15) == 0);
    FUZZ_CHECK(f, ts_usable_size(q) >= size);
    size_t keep = size < s->size ? size : s->size;
    FUZZ_CHECK(f, keep == 0 || memcmp(q, s->shadow, keep) == 0);

    unsigned char *shadow = size ? realloc(s->shadow, size) : NULL;
    if (!size)
        free(s->shadow);
    FUZZ_CHECK(f, size == 0 || shadow != NULL);
    random_bytes(f, shadow + keep, size - keep);
    if (size > keep)
        memcpy(q + keep, shadow + keep, size - keep);
    /* realloc only promises 16-byte alignment, not what the block had. */
    *s = (slot_t){.p = q, .shadow = shadow, .size = size, .align = 0};
}

/* Overwrites a random range of a live block (and its shadow). */
static void op_scribble(fuzz_t *f, slot_t *s) {
    f->what = "scribble";
    if (!s->size)
        return;
    size_t at = (size_t)below(f, s->size);
    size_t n = 1 + (size_t)below(f, s->size - at);
    random_bytes(f, s->shadow + at, n);
    memcpy(s->p + at, s->shadow + at, n);
}

/* Requests that must fail cleanly and leave everything else as it was. */
static void op_bad_args(fuzz_t *f, slot_t *s) {
    void *p = (void *)0x1;
    switch (below(f, 4)) {
    case 0:
        f->what = "calloc overflow";
        errno = 0;
        FUZZ_CHECK(f, calloc_fn(SIZE_MAX / 2 + below(f, 1024), 2 + below(f, 8)) == NULL);
        FUZZ_CHECK(f, errno == ENOMEM);
        break;
    case 1:
        f->what = "posix_memalign bad alignment";
        /* 24, 40, 56, 72 (not powers of two) or 4 (below sizeof(void *)). */
        FUZZ_CHECK(f, memalign_fn(&p, below(f, 2) ? 24 + 16 * below(f, 4) : 4, 64) == EINVAL);
        FUZZ_CHECK(f, p == (void *)0x1);
        break;
    case 2:
        f->what = "posix_memalign too big";
        FUZZ_CHECK(f, memalign_fn(&p, 64, SIZE_MAX - below(f, 4096)) == ENOMEM);
        break;
    default:
        f->what = "realloc too big";
        if (!s->p)
            return;
        errno = 0;
        FUZZ_CHECK(f, realloc_fn(s->p, SIZE_MAX - below(f, 4096)) == NULL);
        FUZZ_CHECK(f, errno == ENOMEM);
        slot_check(f, s); /* untouched on failure */
    }
}

static void fuzz_run(fuzz_t *f, uint64_t seed, long ops) {
    memset(f, 0, sizeof *f);
    f->seed = f->state = seed ? seed : 1;
    for (f->op = 0; f->op < ops && !f->failed; f->op++) {
        slot_t *s = &f->slots[below(f, SLOTS)];
        uint64_t r = below(f, 32);
        if (!s->p) {
            op_alloc(f, s);
        } else if (r < 9) {
            f->what = "free";
            slot_free(f, s);
        } else if (r < 18) {
            op_realloc(f, s);
        } else if (r < 25) {
            op_scribble(f, s);
        } else if (r < 31) {
            f->what = "check";
            slot_check(f, s);
        } else {
            op_bad_args(f, s);
        }
    }
    /* Tear down through the normal path, checking every block one last
     * time. After a failure the table may hold a block that realloc already
     * released, so leak everything rather than risk a double free. */
    f->what = "final free";
    for (int i = 0; i < SLOTS && !f->failed; i++) {
        if (f->slots[i].p)
            slot_free(f, &f->slots[i]);
    }
}

TEST(single) {
    static fuzz_t f;
    fuzz_run(&f, base_seed(), OPS * scale());
    CHECK(!f.failed);
}

typedef struct {
    fuzz_t f;
    uint64_t seed;
} worker_t;

static void *worker(void *arg) {
    worker_t *w = arg;
    fuzz_run(&w->f, w->seed, OPS / 2 * scale());
    return NULL;
}

TEST(threads) {
    static worker_t w[THREADS];
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) {
        w[i].seed = base_seed() + 0x9e3779b97f4a7c15ull * (uint64_t)(i + 1);
        REQUIRE(pthread_create(&t[i], NULL, worker, &w[i]) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], NULL);
        if (w[i].f.failed)
            fprintf(stderr, "  thread %d failed; its seed is 0x%" PRIx64 "\n", i, w[i].seed);
        CHECK(!w[i].f.failed);
    }
}
