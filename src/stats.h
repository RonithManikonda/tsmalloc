#ifndef TS_STATS_H
#define TS_STATS_H

#include "internal.h"

/*
 * Optional counters, compiled in with -DTS_STATS=ON. Without it TS_STAT_ADD
 * expands to nothing, so the fast paths are exactly what they would be if
 * this file didn't exist.
 *
 * With it, every malloc and free does a relaxed atomic add on a shared
 * counter. That is what makes the totals exact, and it also means multi-
 * threaded speed in a stats build says nothing about the real one.
 */
typedef enum {
    TS_STAT_MALLOC,
    TS_STAT_FREE,
    TS_STAT_REMOTE_FREE,
    TS_STAT_PAGE_ALLOC,
    TS_STAT_PAGE_RETIRE,
    TS_STAT_PAGE_PURGE,
    TS_STAT_SEGMENT_MAP,
    TS_STAT_SEGMENT_UNMAP,
    TS_STAT_HUGE_MAP,
    TS_STAT_HUGE_UNMAP,
    TS_STAT_HUGE_CACHE_HIT,
    TS_STAT_COUNT
} ts_stat_t;

#ifdef TS_STATS
#include <stdatomic.h>
extern _Atomic uint64_t ts_stat_counters[TS_STAT_COUNT];
#define TS_STAT_ADD(stat, n) \
    atomic_fetch_add_explicit(&ts_stat_counters[stat], (uint64_t)(n), memory_order_relaxed)
#else
#define TS_STAT_ADD(stat, n) ((void)0)
#endif

#define TS_STAT_INC(stat) TS_STAT_ADD(stat, 1)

/* Current value of a counter; always 0 in a build without stats. */
uint64_t ts_stat_get(ts_stat_t stat);

#endif /* TS_STATS_H */
