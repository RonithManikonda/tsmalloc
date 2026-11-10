/*
 * Replaces the libc allocator in any program started with
 * DYLD_INSERT_LIBRARIES=libtsmalloc.dylib, through dyld's interpose table:
 * each entry below tells dyld to bind other images' references to the second
 * symbol to the first instead.
 *
 * An interposing image's own references aren't rewritten, so a plain call to
 * free or realloc in this file reaches the real one. That is how pointers we
 * didn't allocate are dealt with: anything allocated before the table took
 * effect, or straight from another malloc zone, is handed back to the system.
 *
 * Interposing misses callers that go through the zone API directly (the
 * Objective-C and CoreFoundation runtimes do), so tsmalloc is also registered
 * as the default malloc zone. Between them every route ends up here.
 */
#include "internal.h"
#include "registry.h"
#include "size_class.h"
#include "tsmalloc.h"

#include <errno.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <stdlib.h>
#include <unistd.h>

static void *over_malloc(size_t size) {
    return ts_malloc(size);
}

static void over_free(void *p) {
    if (p == NULL)
        return;
    if (ts_is_ours(p))
        ts_free(p);
    else
        free(p);
}

static void *over_calloc(size_t count, size_t size) {
    return ts_calloc(count, size);
}

static void *over_realloc(void *p, size_t size) {
    if (p == NULL)
        return ts_malloc(size);
    if (ts_is_ours(p))
        return ts_realloc(p, size);
    return realloc(p, size);
}

/* Same as realloc, except that p is released if it fails. */
static void *over_reallocf(void *p, size_t size) {
    void *q = over_realloc(p, size);
    if (q == NULL && size != 0)
        over_free(p);
    return q;
}

static void *over_valloc(size_t size) {
    return ts_aligned_alloc((size_t)getpagesize(), size);
}

static int over_posix_memalign(void **out, size_t alignment, size_t size) {
    return ts_posix_memalign(out, alignment, size);
}

static void *over_aligned_alloc(size_t alignment, size_t size) {
    return ts_aligned_alloc(alignment, size);
}

static size_t over_malloc_size(const void *p) {
    return ts_is_ours(p) ? ts_usable_size(p) : malloc_size(p);
}

typedef struct {
    const void *replacement;
    const void *original;
} interpose_t;

#define INTERPOSE(replacement, original) \
    { (const void *)(replacement), (const void *)(original) }

__attribute__((used, section("__DATA,__interpose")))
static const interpose_t interposers[] = {
    INTERPOSE(over_malloc, malloc),
    INTERPOSE(over_free, free),
    INTERPOSE(over_calloc, calloc),
    INTERPOSE(over_realloc, realloc),
    INTERPOSE(over_reallocf, reallocf),
    INTERPOSE(over_valloc, valloc),
    INTERPOSE(over_posix_memalign, posix_memalign),
    INTERPOSE(over_aligned_alloc, aligned_alloc),
    INTERPOSE(over_malloc_size, malloc_size),
};

/* ---- The malloc zone ------------------------------------------------------ */

/* The system's default zone, which ends up owning anything we didn't make. */
static malloc_zone_t *system_zone;

/* libmalloc asks each zone in turn who owns a pointer, and 0 means "not me". */
static size_t zone_size(malloc_zone_t *zone, const void *p) {
    (void)zone;
    return ts_is_ours(p) ? ts_usable_size(p) : 0;
}

static void *zone_malloc(malloc_zone_t *zone, size_t size) {
    (void)zone;
    return ts_malloc(size);
}

static void *zone_calloc(malloc_zone_t *zone, size_t count, size_t size) {
    (void)zone;
    return ts_calloc(count, size);
}

static void *zone_valloc(malloc_zone_t *zone, size_t size) {
    (void)zone;
    return over_valloc(size);
}

static void zone_free(malloc_zone_t *zone, void *p) {
    (void)zone;
    if (p == NULL)
        return;
    if (ts_is_ours(p))
        ts_free(p);
    else
        malloc_zone_free(system_zone, p);
}

static void *zone_realloc(malloc_zone_t *zone, void *p, size_t size) {
    (void)zone;
    if (p == NULL)
        return ts_malloc(size);
    if (ts_is_ours(p))
        return ts_realloc(p, size);
    return malloc_zone_realloc(system_zone, p, size);
}

static void *zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size) {
    (void)zone;
    return ts_aligned_alloc(alignment, size);
}

static void zone_free_definite_size(malloc_zone_t *zone, void *p, size_t size) {
    (void)size;
    zone_free(zone, p);
}

static void zone_destroy(malloc_zone_t *zone) {
    (void)zone; /* the default zone lives as long as the process */
}

static unsigned zone_batch_malloc(malloc_zone_t *zone, size_t size, void **out, unsigned count) {
    (void)zone;
    for (unsigned i = 0; i < count; i++) {
        out[i] = ts_malloc(size);
        if (out[i] == NULL)
            return i;
    }
    return count;
}

static void zone_batch_free(malloc_zone_t *zone, void **blocks, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        zone_free(zone, blocks[i]);
}

static size_t zone_pressure_relief(malloc_zone_t *zone, size_t goal) {
    (void)zone;
    (void)goal;
    return 0;
}

static boolean_t zone_claimed_address(malloc_zone_t *zone, void *p) {
    (void)zone;
    return ts_is_ours(p);
}

/* What malloc_good_size reports. */
static size_t intro_good_size(malloc_zone_t *zone, size_t size) {
    (void)zone;
    if (size <= TS_MEDIUM_MAX)
        return ts_class_size(ts_size_to_class(size));
    return ts_align_up(size, (size_t)getpagesize());
}

/* Heap inspection isn't supported: tools like `leaks` just see an empty zone. */
static kern_return_t intro_enumerator(task_t task, void *context, unsigned type, vm_address_t zone,
                                      memory_reader_t reader, vm_range_recorder_t recorder) {
    (void)task;
    (void)context;
    (void)type;
    (void)zone;
    (void)reader;
    (void)recorder;
    return KERN_SUCCESS;
}

static boolean_t intro_check(malloc_zone_t *zone) {
    (void)zone;
    return 1;
}

static void intro_print(malloc_zone_t *zone, boolean_t verbose) {
    (void)zone;
    (void)verbose;
}

static void intro_log(malloc_zone_t *zone, void *address) {
    (void)zone;
    (void)address;
}

/* libmalloc calls these around fork for every zone: force_lock before it,
 * force_unlock in the parent, reinit_lock in the child. They are used instead
 * of pthread_atfork because of when they run. In the child, system libraries
 * call malloc before any pthread_atfork handler has had its turn, and with
 * our locks still held that never returns; reinit_lock comes first. */
static void intro_force_lock(malloc_zone_t *zone) {
    (void)zone;
    ts_fork_prepare();
}

static void intro_force_unlock(malloc_zone_t *zone) {
    (void)zone;
    ts_fork_parent();
}

/* libmalloc doesn't check this one for NULL. */
static void intro_reinit_lock(malloc_zone_t *zone) {
    (void)zone;
    ts_fork_child();
}

static void intro_statistics(malloc_zone_t *zone, malloc_statistics_t *stats) {
    (void)zone;
    *stats = (malloc_statistics_t){0};
}

static boolean_t intro_zone_locked(malloc_zone_t *zone) {
    (void)zone;
    return 0;
}

static malloc_introspection_t zone_introspection = {
    .enumerator  = intro_enumerator,
    .good_size   = intro_good_size,
    .check       = intro_check,
    .print       = intro_print,
    .log         = intro_log,
    .force_lock  = intro_force_lock,
    .force_unlock = intro_force_unlock,
    .statistics  = intro_statistics,
    .zone_locked = intro_zone_locked,
    .reinit_lock = intro_reinit_lock,
};

static malloc_zone_t ts_zone = {
    .size               = zone_size,
    .malloc             = zone_malloc,
    .calloc             = zone_calloc,
    .valloc             = zone_valloc,
    .free               = zone_free,
    .realloc            = zone_realloc,
    .destroy            = zone_destroy,
    .zone_name          = "tsmalloc",
    .batch_malloc       = zone_batch_malloc,
    .batch_free         = zone_batch_free,
    .introspect         = &zone_introspection,
    .version            = 10, /* has everything up to claimed_address */
    .memalign           = zone_memalign,
    .free_definite_size = zone_free_definite_size,
    .pressure_relief    = zone_pressure_relief,
    .claimed_address    = zone_claimed_address,
};

/* malloc_default_zone() can return a wrapper; the first entry of the zone
 * list is the real one. */
static malloc_zone_t *current_default_zone(void) {
    vm_address_t *zones = NULL;
    unsigned count = 0;
    if (malloc_get_all_zones(mach_task_self(), NULL, &zones, &count) == KERN_SUCCESS && count > 0)
        return (malloc_zone_t *)zones[0];
    return malloc_default_zone();
}

__attribute__((constructor)) static void zone_install(void) {
    system_zone = current_default_zone();
    /* Created on demand, so make sure it exists before reordering. */
    malloc_zone_t *purgeable = malloc_default_purgeable_zone();

    /* The default zone is simply the first one registered, and unregistering
     * moves the last zone into the gap. So: add ours at the end, take the old
     * default out so ours lands first, then add the old default back so its
     * existing blocks can still be found and freed. */
    malloc_zone_register(&ts_zone);
    ts_fork_via_zone = 1;
    malloc_zone_unregister(system_zone);
    malloc_zone_register(system_zone);
    /* Keep the purgeable zone behind the system one, where it started. */
    if (purgeable != NULL) {
        malloc_zone_unregister(purgeable);
        malloc_zone_register(purgeable);
    }
}
