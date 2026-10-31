#include "test.h"

#include "segment.h"
#include "tsmalloc.h"

#include <pthread.h>
#include <stdatomic.h>

/* Multithreaded stress through the public API only.
 *
 * Worker threads keep a private set of live blocks and do random
 * operations on them: malloc, calloc, aligned_alloc, posix_memalign,
 * realloc, free. Every block carries a header (size + pattern byte) and is
 * filled with its pattern, and is checked before every realloc and free.
 * A shared table lets blocks change hands: a worker swaps one of its blocks
 * for whatever another worker left there, so blocks get realloc'd and freed
 * by threads that didn't allocate them, often after the allocating thread
 * has exited (workers run in waves, and the table outlives each wave).
 *
 * TS_TEST_SCALE multiplies the operations per worker. Under TSan and ASan
 * the default scale takes a few seconds. */

enum { WORKERS = 8, WAVES = 6, LOCAL = 256, SHARED = 1024, OPS = 12000 };

typedef struct {
    uint32_t size;    /* bytes requested, including this header */
    uint8_t pattern;  /* every byte after the header */
    uint8_t pad[11];
} hdr_t;

_Static_assert(sizeof(hdr_t) == 16, "header keeps the body 16-aligned");

static _Atomic(uintptr_t) shared[SHARED];
static _Atomic long errors;
static long ops_per_worker;

static long scale(void) {
    const char *s = getenv("TS_TEST_SCALE");
    long v = s ? atol(s) : 1;
    return v > 0 ? v : 1;
}

static uint32_t rnd(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

/* Mostly small, some medium, the odd large block (both sides of every
 * size-class and small/large boundary get hit). */
static size_t pick_size(uint32_t *rs) {
    uint32_t r = rnd(rs);
    size_t size;
    switch (r & 15) {
    case 0:
        size = 16384 + (r >> 8) % 200000;
        break;
    case 1: case 2: case 3:
        size = 1024 + (r >> 8) % 15360;
        break;
    default:
        size = (r >> 8) % 1024;
    }
    return size < sizeof(hdr_t) ? sizeof(hdr_t) : size;
}

static void fail(const char *what) {
    fprintf(stderr, "  stress: %s\n", what);
    atomic_fetch_add(&errors, 1);
}

static void fill(unsigned char *p, size_t size, uint8_t pattern) {
    hdr_t h = {.size = (uint32_t)size, .pattern = pattern};
    memcpy(p, &h, sizeof h);
    memset(p + sizeof h, pattern, size - sizeof h);
}

/* Big blocks are checked at both ends and a few places in between; that
 * catches overlap without making large blocks dominate the run time. */
static int intact(const unsigned char *p) {
    hdr_t h;
    memcpy(&h, p, sizeof h);
    if (h.size < sizeof h || h.size > 16384 + 200000)
        return 0;
    if (ts_usable_size(p) < h.size)
        return 0;
    size_t body = h.size - sizeof h;
    const unsigned char *b = p + sizeof h;
    if (body <= 4096) {
        for (size_t i = 0; i < body; i++)
            if (b[i] != h.pattern)
                return 0;
        return 1;
    }
    for (size_t i = 0; i < body; i += body / 16)
        if (b[i] != h.pattern)
            return 0;
    return b[body - 1] == h.pattern && b[0] == h.pattern;
}

static unsigned char *alloc_block(uint32_t *rs) {
    size_t size = pick_size(rs);
    uint8_t pattern = (uint8_t)(rnd(rs) | 1);
    unsigned char *p = NULL;
    switch (rnd(rs) % 6) {
    case 0: {
        p = ts_calloc(1, size);
        if (p != NULL)
            for (size_t i = 0; i < size; i += 61)
                if (p[i] != 0) {
                    fail("calloc block not zeroed");
                    break;
                }
        break;
    }
    case 1: {
        size_t align = (size_t)32 << (rnd(rs) % 8); /* 32 B .. 4 KiB */
        p = ts_aligned_alloc(align, size);
        if (p != NULL && (uintptr_t)p % align != 0)
            fail("aligned_alloc misaligned");
        break;
    }
    case 2: {
        size_t align = (size_t)16 << (rnd(rs) % 4);
        void *q;
        if (ts_posix_memalign(&q, align, size) == 0) {
            p = q;
            if ((uintptr_t)p % align != 0)
                fail("posix_memalign misaligned");
        }
        break;
    }
    default:
        p = ts_malloc(size);
    }
    if (p == NULL) {
        fail("allocation returned NULL");
        return NULL;
    }
    if ((uintptr_t)p % 16 != 0)
        fail("block not 16-aligned");
    fill(p, size, pattern);
    return p;
}

static void free_block(unsigned char *p) {
    if (!intact(p))
        fail("block corrupted before free");
    ts_free(p);
}

/* Grow or shrink, keeping the old contents up to the smaller size. */
static unsigned char *realloc_block(unsigned char *p, uint32_t *rs) {
    if (!intact(p)) {
        fail("block corrupted before realloc");
        return p;
    }
    hdr_t h;
    memcpy(&h, p, sizeof h);
    size_t size = pick_size(rs);
    unsigned char *q = ts_realloc(p, size);
    if (q == NULL) {
        fail("realloc returned NULL");
        return p;
    }
    size_t keep = size < h.size ? size : h.size;
    for (size_t i = sizeof h; i < keep; i += 13)
        if (q[i] != h.pattern) {
            fail("realloc lost contents");
            break;
        }
    fill(q, size, h.pattern);
    return q;
}

static void *worker(void *arg) {
    uint32_t rs = (uint32_t)(uintptr_t)arg * 2654435761u | 1;
    unsigned char *mine[LOCAL] = {0};
    for (long op = 0; op < ops_per_worker; op++) {
        uint32_t r = rnd(&rs);
        unsigned char **slot = &mine[(r >> 4) % LOCAL];
        switch (r & 15) {
        case 0: case 1: {
            /* Trade with the shared table: our block goes there, whatever
             * was there (any thread's, maybe a dead one's) becomes ours. */
            size_t i = (r >> 12) % SHARED;
            *slot = (unsigned char *)atomic_exchange(&shared[i], (uintptr_t)*slot);
            break;
        }
        case 2: case 3:
            if (*slot != NULL)
                *slot = realloc_block(*slot, &rs);
            break;
        default:
            /* Replace: this is what gives blocks random lifetimes. */
            if (*slot != NULL)
                free_block(*slot);
            *slot = (r & 64) ? alloc_block(&rs) : NULL;
        }
    }
    /* Leave about half behind in the table for the next wave. */
    for (size_t i = 0; i < LOCAL; i++) {
        if (mine[i] == NULL)
            continue;
        if (i & 1)
            mine[i] = (unsigned char *)atomic_exchange(&shared[(i * 7 + (uintptr_t)arg) % SHARED],
                                                       (uintptr_t)mine[i]);
        if (mine[i] != NULL)
            free_block(mine[i]);
    }
    return NULL;
}

TEST(stress_random_ops_across_threads_and_waves) {
    ops_per_worker = OPS * scale();
    size_t mapped_after_first = 0;
    for (int wave = 0; wave < WAVES; wave++) {
        pthread_t t[WORKERS];
        for (uintptr_t i = 0; i < WORKERS; i++)
            REQUIRE(pthread_create(&t[i], NULL, worker, (void *)(wave * WORKERS + i + 1)) == 0);
        for (int i = 0; i < WORKERS; i++)
            pthread_join(t[i], NULL);
        if (wave == 0)
            mapped_after_first = ts_segment_count();
    }
    /* The main thread frees what the last wave left, all of it remote. */
    for (size_t i = 0; i < SHARED; i++) {
        unsigned char *p = (unsigned char *)atomic_exchange(&shared[i], 0);
        if (p != NULL)
            free_block(p);
    }
    CHECK_EQ(atomic_load(&errors), 0);
    /* Later waves adopt what earlier ones left instead of mapping more.
     * Per-wave memory is about the same, so allow some slack, not growth
     * proportional to the number of waves. */
    CHECK(ts_segment_count() <= 2 * mapped_after_first + 4);
}

/* realloc and free of blocks whose owner is alive and busy allocating:
 * one producer, several threads that grow every block before freeing it. */
enum { HANDOFF = 4096 };
static _Atomic(uintptr_t) handoff[HANDOFF];
static _Atomic int producing;

static void *producer(void *arg) {
    uint32_t rs = 0x9e3779b9u;
    long n = (long)(intptr_t)arg;
    for (long i = 0; i < n; i++) {
        unsigned char *p = alloc_block(&rs);
        size_t slot = rnd(&rs) % HANDOFF;
        unsigned char *old = (unsigned char *)atomic_exchange(&handoff[slot], (uintptr_t)p);
        if (old != NULL)
            free_block(old);
    }
    atomic_store(&producing, 0);
    return NULL;
}

static void *regrower(void *arg) {
    uint32_t rs = (uint32_t)(uintptr_t)arg | 1;
    while (atomic_load(&producing)) {
        size_t slot = rnd(&rs) % HANDOFF;
        unsigned char *p = (unsigned char *)atomic_exchange(&handoff[slot], 0);
        if (p == NULL)
            continue;
        p = realloc_block(p, &rs);
        free_block(p);
    }
    return NULL;
}

TEST(stress_remote_realloc_while_owner_allocates) {
    enum { REGROWERS = 5 };
    atomic_store(&producing, 1);
    pthread_t prod, rg[REGROWERS];
    for (uintptr_t i = 0; i < REGROWERS; i++)
        REQUIRE(pthread_create(&rg[i], NULL, regrower, (void *)(i + 7)) == 0);
    REQUIRE(pthread_create(&prod, NULL, producer, (void *)(intptr_t)(40000 * scale())) == 0);
    pthread_join(prod, NULL);
    for (int i = 0; i < REGROWERS; i++)
        pthread_join(rg[i], NULL);
    for (size_t i = 0; i < HANDOFF; i++) {
        unsigned char *p = (unsigned char *)atomic_exchange(&handoff[i], 0);
        if (p != NULL)
            free_block(p);
    }
    CHECK_EQ(atomic_load(&errors), 0);
}
