#include "huge.h"

#include "os.h"
#include "registry.h"
#include "segment.h"
#include "stats.h"

#include <errno.h>
#include <pthread.h>

/* Bucket b holds mappings of 2^(b + TS_HUGE_MIN_SHIFT) bytes up to just
 * under twice that. A huge mapping is always over 512 KiB, so 2^19 is the
 * bottom; the top bucket exists for exactly TS_HUGE_CACHE_MAX_SIZE. */
#define TS_HUGE_MIN_SHIFT 19
#define TS_HUGE_BUCKETS   8

_Static_assert(((size_t)1 << TS_HUGE_MIN_SHIFT) <= TS_MEDIUM_MAX, "huge sizes start in bucket 0");
_Static_assert(TS_HUGE_CACHE_MAX_SIZE >> (TS_HUGE_MIN_SHIFT + TS_HUGE_BUCKETS - 1) == 1,
               "the largest cacheable size lands in the top bucket");

typedef struct {
    void *base;
    size_t mapped;
    uint64_t stamp; /* when it was cached; the lowest stamp is evicted first */
    int hot;        /* still resident: not decommitted yet */
} cache_entry_t;

typedef struct {
    cache_entry_t slots[TS_HUGE_BUCKET_SLOTS];
    unsigned count;
} cache_bucket_t;

/* One lock is plenty: every path through here is about to make, or has just
 * saved, a system call, and the critical sections are a few dozen loads. */
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static cache_bucket_t cache[TS_HUGE_BUCKETS];
static size_t cache_bytes;
static size_t cache_count;
static size_t cache_hot_bytes; /* the part of cache_bytes that is resident */
static uint64_t cache_clock;

/* Everything that could be evicted by one insert, plus the odd one out. */
#define TS_HUGE_MAX_VICTIMS (TS_HUGE_BUCKETS * TS_HUGE_BUCKET_SLOTS + 1)

static unsigned bucket_of(size_t mapped) {
    return ts_bsr(mapped) - TS_HUGE_MIN_SHIFT;
}

static int cacheable(size_t mapped) {
    return mapped >= ((size_t)1 << TS_HUGE_MIN_SHIFT) && mapped <= TS_HUGE_CACHE_MAX_SIZE;
}

/* Remove and return one slot. Caller holds the lock. */
static cache_entry_t cache_take(cache_bucket_t *b, unsigned i) {
    cache_entry_t e = b->slots[i];
    b->slots[i] = b->slots[--b->count];
    cache_bytes -= e.mapped;
    cache_count--;
    if (e.hot)
        cache_hot_bytes -= e.mapped;
    return e;
}

/* A cached mapping of at least `need` bytes that wastes at most a quarter of
 * itself, or NULL. Such a mapping is in the bucket of `need` or the one
 * above. Takes the tightest fit, so the big ones stay for big requests. */
static void *cache_get(size_t need, size_t *mapped) {
    if (!cacheable(need))
        return NULL;
    unsigned first = bucket_of(need);
    void *base = NULL;

    pthread_mutex_lock(&cache_lock);
    cache_bucket_t *best_bucket = NULL;
    unsigned best_slot = 0;
    size_t best = SIZE_MAX;
    for (unsigned bi = first; bi <= first + 1 && bi < TS_HUGE_BUCKETS; bi++) {
        cache_bucket_t *b = &cache[bi];
        for (unsigned i = 0; i < b->count; i++) {
            size_t m = b->slots[i].mapped;
            if (m >= need && m - need <= m / 4 && m < best) {
                best = m;
                best_bucket = b;
                best_slot = i;
            }
        }
    }
    if (best_bucket != NULL) {
        cache_entry_t e = cache_take(best_bucket, best_slot);
        base = e.base;
        *mapped = e.mapped;
    }
    pthread_mutex_unlock(&cache_lock);
    return base;
}

/* Oldest entry in `b`. The bucket must not be empty. */
static unsigned oldest_slot(const cache_bucket_t *b) {
    unsigned oldest = 0;
    for (unsigned i = 1; i < b->count; i++)
        if (b->slots[i].stamp < b->slots[oldest].stamp)
            oldest = i;
    return oldest;
}

/* Oldest entry anywhere. The cache must not be empty. */
static cache_bucket_t *oldest_bucket(void) {
    cache_bucket_t *found = NULL;
    uint64_t stamp = UINT64_MAX;
    for (unsigned bi = 0; bi < TS_HUGE_BUCKETS; bi++) {
        cache_bucket_t *b = &cache[bi];
        if (b->count == 0)
            continue;
        uint64_t s = b->slots[oldest_slot(b)].stamp;
        if (s < stamp) {
            stamp = s;
            found = b;
        }
    }
    return found;
}

/* Decommit the oldest resident mappings until at most TS_HUGE_HOT_BYTES stay
 * resident. This has to happen under the lock: once a mapping can be found
 * in the cache another thread may take it and write to it, and an madvise
 * landing after that would throw its data away. It is also rare, because a
 * program that keeps reusing the same few mappings never gets here. */
static void cache_trim_hot(void) {
    while (cache_hot_bytes > TS_HUGE_HOT_BYTES) {
        cache_entry_t *oldest = NULL;
        for (unsigned bi = 0; bi < TS_HUGE_BUCKETS; bi++)
            for (unsigned i = 0; i < cache[bi].count; i++) {
                cache_entry_t *e = &cache[bi].slots[i];
                if (e->hot && (oldest == NULL || e->stamp < oldest->stamp))
                    oldest = e;
            }
        ts_os_decommit(oldest->base, oldest->mapped);
        oldest->hot = 0;
        cache_hot_bytes -= oldest->mapped;
    }
}

static void cache_put(void *base, size_t mapped) {
    /* Evicted mappings are unmapped after the lock is dropped: munmap can
     * take far longer than anything else done in here. */
    cache_entry_t victims[TS_HUGE_MAX_VICTIMS];
    size_t nvictims = 0;

    pthread_mutex_lock(&cache_lock);
    cache_bucket_t *b = &cache[bucket_of(mapped)];
    if (b->count == TS_HUGE_BUCKET_SLOTS)
        victims[nvictims++] = cache_take(b, oldest_slot(b));
    while (cache_bytes + mapped > TS_HUGE_CACHE_MAX_BYTES) {
        cache_bucket_t *old = oldest_bucket();
        victims[nvictims++] = cache_take(old, oldest_slot(old));
    }
    b->slots[b->count++] = (cache_entry_t){base, mapped, ++cache_clock, 1};
    cache_bytes += mapped;
    cache_count++;
    cache_hot_bytes += mapped;
    cache_trim_hot();
    pthread_mutex_unlock(&cache_lock);

    for (size_t i = 0; i < nvictims; i++) {
        ts_os_free(victims[i].base, victims[i].mapped);
        TS_STAT_INC(TS_STAT_HUGE_UNMAP);
    }
}

static void *finish_block(char *base, size_t lead, size_t mapped) {
    char *user = base + lead;
    /* Normally the header is at `base`, but for alignments above the segment
     * size it goes in the segment-sized chunk just before the user pointer,
     * which is where a mask of `user - 1` lands. */
    ts_huge_t *h = (ts_huge_t *)ts_ptr_segment(user - 1);
    h->kind = TS_KIND_LARGE;
    h->lead = (uint32_t)lead;
    h->mapped = mapped;
    if (!ts_registry_add(h)) {
        ts_os_free(base, mapped);
        return NULL; /* errno is ENOMEM */
    }
    return user;
}

static void *huge_map(size_t size, size_t align, int may_recycle) {
    size_t lead = align > sizeof(ts_huge_t) ? align : sizeof(ts_huge_t);
    if (align > ((size_t)1 << 31) || size > SIZE_MAX - lead - TS_SEGMENT_SIZE) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = lead + size;
    /* The mapping is rounded up to whole OS pages; the tail is ours to hand
     * out, which is what makes realloc-in-place possible. */
    size_t mapped = ts_align_up(total, ts_os_page_size());

    /* Cached mappings are aligned to the segment size, so they suit any
     * alignment up to that. Bigger alignments are rare enough to always map. */
    if (may_recycle && align <= TS_SEGMENT_SIZE) {
        size_t cached;
        void *base = cache_get(mapped, &cached);
        if (base != NULL) {
            TS_STAT_INC(TS_STAT_HUGE_CACHE_HIT);
            return finish_block(base, lead, cached);
        }
    }

    size_t map_align = align > TS_SEGMENT_SIZE ? align : TS_SEGMENT_SIZE;
    char *base = ts_os_alloc_aligned(total, map_align);
    if (base == NULL)
        return NULL;
    TS_STAT_INC(TS_STAT_HUGE_MAP);
    return finish_block(base, lead, mapped);
}

void *ts_huge_alloc(size_t size, size_t align) {
    return huge_map(size, align, 1);
}

void *ts_huge_alloc_zeroed(size_t size, size_t align) {
    return huge_map(size, align, 0);
}

void ts_huge_free(ts_huge_t *h, void *p) {
    size_t lead = h->lead;
    size_t mapped = h->mapped;
    char *base = (char *)p - lead;

    ts_registry_remove(h);
    if (lead > TS_SEGMENT_SIZE || !cacheable(mapped)) {
        ts_os_free(base, mapped);
        TS_STAT_INC(TS_STAT_HUGE_UNMAP);
        return;
    }
    cache_put(base, mapped);
}

void ts_huge_shrink(ts_huge_t *h, void *p, size_t size) {
    size_t keep = ts_align_up(h->lead + size, ts_os_page_size());
    if (keep < h->mapped) {
        char *base = (char *)p - h->lead;
        ts_os_free(base + keep, h->mapped - keep);
        h->mapped = keep;
    }
}

void ts_huge_cache_info(size_t *count, size_t *bytes, size_t *hot_bytes) {
    pthread_mutex_lock(&cache_lock);
    *count = cache_count;
    *bytes = cache_bytes;
    *hot_bytes = cache_hot_bytes;
    pthread_mutex_unlock(&cache_lock);
}

void ts_huge_cache_flush(void) {
    for (;;) {
        pthread_mutex_lock(&cache_lock);
        cache_bucket_t *b = cache_count != 0 ? oldest_bucket() : NULL;
        cache_entry_t e = {0};
        if (b != NULL)
            e = cache_take(b, oldest_slot(b));
        pthread_mutex_unlock(&cache_lock);
        if (b == NULL)
            return;
        ts_os_free(e.base, e.mapped);
        TS_STAT_INC(TS_STAT_HUGE_UNMAP);
    }
}

void ts_huge_fork_prepare(void) {
    pthread_mutex_lock(&cache_lock);
}

void ts_huge_fork_parent(void) {
    pthread_mutex_unlock(&cache_lock);
}

void ts_huge_fork_child(void) {
    cache_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER; /* see ts_heap_fork_child */
}
