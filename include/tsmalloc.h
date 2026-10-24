#ifndef TSMALLOC_H
#define TSMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same contracts as the standard functions of the same name. */
void  *ts_malloc(size_t size);
void   ts_free(void *p);
void  *ts_calloc(size_t count, size_t size);
void  *ts_realloc(void *p, size_t size);
void  *ts_aligned_alloc(size_t alignment, size_t size);
int    ts_posix_memalign(void **out, size_t alignment, size_t size);

/* Number of bytes actually usable at p (>= the size that was requested).
 * Returns 0 for NULL. */
size_t ts_usable_size(const void *p);

#ifdef __cplusplus
}
#endif

#endif /* TSMALLOC_H */
