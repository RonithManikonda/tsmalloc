#if defined(__linux__)
#define _GNU_SOURCE /* MAP_ANONYMOUS, MADV_* under -std=c11 */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "os.h"

#include "internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* Cached because sysconf is a libc call on every malloc slow path otherwise.
 * Racing initialisers all store the same value, so relaxed is enough. */
static _Atomic size_t os_page_size;

size_t ts_os_page_size(void) {
    size_t ps = atomic_load_explicit(&os_page_size, memory_order_relaxed);
    if (ps == 0) {
        long r = sysconf(_SC_PAGESIZE);
        ps = r > 0 ? (size_t)r : 4096;
        atomic_store_explicit(&os_page_size, ps, memory_order_relaxed);
    }
    return ps;
}

/* Returns 0 if rounding `n` up to `align` would overflow. */
static size_t round_up_checked(size_t n, size_t align) {
    if (n > SIZE_MAX - (align - 1))
        return 0;
    return ts_align_up(n, align);
}

static void *os_map(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        errno = ENOMEM;
        return NULL;
    }
    return p;
}

static void os_unmap(void *p, size_t size) {
    if (size == 0)
        return;
    if (munmap(p, size) != 0) {
        /* Only fails on a bad range, which means our own bookkeeping is
         * broken. Don't call anything that might allocate. */
        static const char msg[] = "tsmalloc: munmap failed\n";
        (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
        abort();
    }
}

void *ts_os_alloc(size_t size) {
    size = round_up_checked(size, ts_os_page_size());
    if (size == 0) {
        errno = ENOMEM;
        return NULL;
    }
    return os_map(size);
}

void *ts_os_alloc_aligned(size_t size, size_t align) {
    size_t ps = ts_os_page_size();
    if (align <= ps)
        return ts_os_alloc(size);

    size = round_up_checked(size, ps);
    if (size == 0 || size > SIZE_MAX - align) {
        errno = ENOMEM;
        return NULL;
    }

    /* The kernel often hands out addresses that happen to be aligned
     * already (consecutive mappings tend to be adjacent), so try the cheap
     * way first. */
    void *p = os_map(size);
    if (p == NULL)
        return NULL;
    if (((uintptr_t)p & (align - 1)) == 0)
        return p;
    os_unmap(p, size);

    /* Reserve size + align so an aligned start must exist inside, then give
     * the slop on both sides back. Both pieces are page multiples because
     * size, align and the mapping start all are. */
    size_t over = size + align;
    char *raw = os_map(over);
    if (raw == NULL)
        return NULL;
    char *aligned = (char *)ts_align_up((uintptr_t)raw, align);
    size_t head = (size_t)(aligned - raw);
    size_t tail = over - head - size;
    os_unmap(raw, head);
    os_unmap(aligned + size, tail);
    return aligned;
}

void ts_os_free(void *p, size_t size) {
    if (p == NULL)
        return;
    os_unmap(p, ts_align_up(size, ts_os_page_size()));
}

void ts_os_decommit(void *p, size_t size) {
    size_t ps = ts_os_page_size();
    uintptr_t start = ts_align_up((uintptr_t)p, ps);
    uintptr_t end = ts_align_down((uintptr_t)p + size, ps);
    if (end <= start)
        return;

    /* MADV_FREE is lazy: the kernel only takes the pages under memory
     * pressure, and touching them again before that is free. Older Linux
     * kernels reject it with EINVAL, so fall back to MADV_DONTNEED (pages
     * are dropped immediately and read back as zero). Failure is harmless:
     * the memory just stays resident. */
#ifdef MADV_FREE
    if (madvise((void *)start, end - start, MADV_FREE) == 0)
        return;
#endif
    (void)madvise((void *)start, end - start, MADV_DONTNEED);
}
