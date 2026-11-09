#include "stats.h"

#include "tsmalloc.h"

#include <stdio.h>

#ifdef TS_STATS
_Atomic uint64_t ts_stat_counters[TS_STAT_COUNT];

uint64_t ts_stat_get(ts_stat_t stat) {
    return atomic_load_explicit(&ts_stat_counters[stat], memory_order_relaxed);
}

void ts_stats_print(FILE *out) {
    static const char *const names[TS_STAT_COUNT] = {
        [TS_STAT_MALLOC]         = "mallocs",
        [TS_STAT_FREE]           = "frees",
        [TS_STAT_REMOTE_FREE]    = "remote frees",
        [TS_STAT_PAGE_ALLOC]     = "pages allocated",
        [TS_STAT_PAGE_RETIRE]    = "pages retired",
        [TS_STAT_PAGE_PURGE]     = "pages purged",
        [TS_STAT_SEGMENT_MAP]    = "segments mapped",
        [TS_STAT_SEGMENT_UNMAP]  = "segments unmapped",
        [TS_STAT_HUGE_MAP]       = "huge mmaps",
        [TS_STAT_HUGE_UNMAP]     = "huge munmaps",
        [TS_STAT_HUGE_CACHE_HIT] = "huge cache hits",
    };
    fprintf(out, "tsmalloc stats:\n");
    for (int i = 0; i < TS_STAT_COUNT; i++)
        fprintf(out, "  %-18s %llu\n", names[i], (unsigned long long)ts_stat_get((ts_stat_t)i));
}
#else
uint64_t ts_stat_get(ts_stat_t stat) {
    (void)stat;
    return 0;
}

void ts_stats_print(FILE *out) {
    fprintf(out, "tsmalloc stats: not compiled in (configure with -DTS_STATS=ON)\n");
}
#endif
