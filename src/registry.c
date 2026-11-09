#include "registry.h"

#include "huge.h"
#include "os.h"
#include "segment.h"

#include <stdatomic.h>

#define CHUNK_BITS ((size_t)1 << (TS_REGISTRY_ADDRESS_BITS - TS_SEGMENT_SHIFT))
#define TABLE_WORDS (CHUNK_BITS / 64)
#define TABLE_BYTES (TABLE_WORDS * sizeof(uint64_t))

static _Atomic(_Atomic uint64_t *) table;

static _Atomic uint64_t *table_get_or_map(void) {
    _Atomic uint64_t *t = atomic_load_explicit(&table, memory_order_acquire);
    if (ts_likely(t != NULL))
        return t;
    _Atomic uint64_t *fresh = ts_os_alloc(TABLE_BYTES);
    if (fresh == NULL)
        return NULL;
    /* Two first callers both map one; the loser gives its copy back. Fresh
     * mappings are zero, which is an empty table. */
    if (atomic_compare_exchange_strong_explicit(&table, &t, fresh, memory_order_acq_rel,
                                                memory_order_acquire))
        return fresh;
    ts_os_free((void *)fresh, TABLE_BYTES);
    return t;
}

TS_INLINE size_t chunk_index(const void *p) {
    return (uintptr_t)p >> TS_SEGMENT_SHIFT;
}

int ts_registry_add(const void *region) {
    _Atomic uint64_t *t = table_get_or_map();
    if (t == NULL)
        return 0;
    size_t i = chunk_index(region);
    /* Release pairs with the acquire in chunk_is_ours: whoever sees the bit
     * also sees the region's header, written before this. */
    atomic_fetch_or_explicit(&t[i / 64], (uint64_t)1 << (i % 64), memory_order_release);
    return 1;
}

void ts_registry_remove(const void *region) {
    _Atomic uint64_t *t = atomic_load_explicit(&table, memory_order_acquire);
    size_t i = chunk_index(region);
    atomic_fetch_and_explicit(&t[i / 64], ~((uint64_t)1 << (i % 64)), memory_order_release);
}

static int chunk_is_ours(const void *p) {
    if ((uintptr_t)p >> TS_REGISTRY_ADDRESS_BITS != 0)
        return 0;
    _Atomic uint64_t *t = atomic_load_explicit(&table, memory_order_acquire);
    if (t == NULL)
        return 0;
    size_t i = chunk_index(p);
    return (atomic_load_explicit(&t[i / 64], memory_order_acquire) >> (i % 64)) & 1;
}

int ts_is_ours(const void *p) {
    if (p == NULL)
        return 0;
    /* Free finds a region by masking p - 1 (see tsmalloc.c), so that is the
     * chunk to look up. */
    const char *last = (const char *)p - 1;
    if (!chunk_is_ours(last))
        return 0;

    /* A set bit alone isn't enough, for two reasons. p - 1 lands in the chunk
     * before p when p is chunk aligned, and that chunk may be ours while p
     * is somebody else's. And a huge mapping under 4 MiB (or one shrunk by
     * realloc) doesn't cover its chunk, so another mapping can sit in the
     * rest of it. The header settles both. */
    const ts_segment_t *region = ts_ptr_segment(last);
    if (region->kind == TS_KIND_SEGMENT)
        return ((uintptr_t)p & TS_SEGMENT_MASK) != 0;
    if (region->kind != TS_KIND_LARGE)
        return 0;

    /* The one address a huge block can have, given where its header is. With
     * a lead above the segment size the header is the chunk before p. */
    const ts_huge_t *h = (const ts_huge_t *)region;
    size_t offset = h->lead > TS_SEGMENT_SIZE ? TS_SEGMENT_SIZE : h->lead;
    return (const char *)p == (const char *)h + offset;
}
