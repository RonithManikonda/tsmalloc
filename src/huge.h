#ifndef TS_HUGE_H
#define TS_HUGE_H

#include "internal.h"

/*
 * Blocks above TS_MEDIUM_MAX: each gets a mapping of its own, aligned to the
 * segment size, with a ts_huge_t in front so a mask of the pointer finds it.
 * The mapping reserves whole segment-sized chunks; `mapped` is the part in
 * use, and the rest of the last chunk is never touched.
 *
 * mmap and munmap are slow and every new page faults, so freed mappings go
 * into a small process-wide cache instead of straight back to the OS. It is
 * bucketed by size and capped in bytes.
 *
 * Only the oldest cached mappings are decommitted (MADV_FREE). Doing it on
 * every free costs a system call plus a page fault per page on the next use,
 * which is most of what the cache is there to save, so the most recent
 * TS_HUGE_HOT_BYTES stay resident and ready.
 */
typedef struct {
    uint32_t kind;   /* TS_KIND_LARGE */
    uint32_t lead;   /* user pointer - start of the mapping */
    size_t   mapped; /* bytes mapped, a multiple of the OS page size */
} ts_huge_t;

_Static_assert(sizeof(ts_huge_t) == TS_ALIGN, "huge header keeps user pointers aligned");

/* Largest mapping the cache takes, and how much it holds in total. Each
 * size bucket (one per power of two) keeps TS_HUGE_BUCKET_SLOTS mappings. */
#define TS_HUGE_CACHE_MAX_SIZE  ((size_t)64 << 20)
#define TS_HUGE_CACHE_MAX_BYTES ((size_t)128 << 20)
#define TS_HUGE_BUCKET_SLOTS    4

/* How much of the cache may stay resident, not decommitted. */
#define TS_HUGE_HOT_BYTES       ((size_t)32 << 20)

/* `size` bytes aligned to `align` (a power of two >= TS_ALIGN), or NULL with
 * errno set. Contents are indeterminate; may be a recycled mapping. */
void *ts_huge_alloc(size_t size, size_t align);

/* The same, but all zero. Always a fresh mapping: clearing a recycled one
 * would fault in every page, where fresh ones stay untouched until used. */
void *ts_huge_alloc_zeroed(size_t size, size_t align);

/* Free the block at `p`, whose header is `h`. */
void ts_huge_free(ts_huge_t *h, void *p);

/* Give back the whole OS pages past the first `size` bytes, in place: whole
 * chunks are unmapped, the rest of the last chunk is decommitted. */
void ts_huge_shrink(ts_huge_t *h, void *p, size_t size);

TS_INLINE size_t ts_huge_usable(const ts_huge_t *h) {
    return h->mapped - h->lead;
}

/* Current contents of the cache, and a way to empty it (for tests). */
void ts_huge_cache_info(size_t *count, size_t *bytes, size_t *hot_bytes);
void ts_huge_cache_flush(void);

/* pthread_atfork handlers for the cache lock. */
void ts_huge_fork_prepare(void);
void ts_huge_fork_parent(void);
void ts_huge_fork_child(void);

#endif /* TS_HUGE_H */
