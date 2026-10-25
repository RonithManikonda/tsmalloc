#ifndef TSMALLOC_H
#define TSMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same contracts as the standard functions of the same name. malloc(0)
 * returns a unique, freeable pointer. */
void  *ts_malloc(size_t size);
void   ts_free(void *p);
void  *ts_calloc(size_t count, size_t size);

/* realloc(NULL, n) is malloc(n). realloc(p, 0) behaves like realloc(p, 1):
 * p is released and a minimal block is returned (the macOS behaviour, not
 * glibc's free-and-return-NULL), so NULL from realloc always means failure.
 * On failure NULL is returned, errno is ENOMEM and p is left untouched. */
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
