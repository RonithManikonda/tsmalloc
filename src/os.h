#ifndef TS_OS_H
#define TS_OS_H

#include <stddef.h>

/* Thin layer over the virtual memory API. Nothing here calls malloc, so it is
 * safe to use from inside the allocator. All sizes are rounded up to whole
 * OS pages. */

/* OS page size (16 KiB on Apple silicon, usually 4 KiB on Linux). */
size_t ts_os_page_size(void);

/* Fresh zeroed read/write memory, page aligned. NULL (errno = ENOMEM) on failure. */
void *ts_os_alloc(size_t size);

/* Like ts_os_alloc but aligned to `align`, which must be a power of two.
 * Alignments above the page size are done by over-reserving and unmapping
 * the unaligned head and tail, so nothing extra stays mapped. */
void *ts_os_alloc_aligned(size_t size, size_t align);

/* Return a region from ts_os_alloc / ts_os_alloc_aligned. `size` is the size
 * that was requested. */
void ts_os_free(void *p, size_t size);

/* Tell the OS it may reclaim the physical pages backing [p, p + size) while
 * keeping the range mapped. Later reads return either the old contents or
 * zeros, so callers must treat the memory as uninitialised. Only whole pages
 * inside the range are affected. */
void ts_os_decommit(void *p, size_t size);

#endif /* TS_OS_H */
