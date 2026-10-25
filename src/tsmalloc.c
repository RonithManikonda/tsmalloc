/*
 * Public entry points.
 *
 * Every pointer we hand out has a 16-byte header right in front of it that
 * says where the memory came from:
 *
 *   - large (> TS_SMALL_MAX): our own mmap'd region, unmapped on free;
 *   - small: still forwarded to the system allocator until the page-based
 *     small allocator replaces it.
 *
 * Keeping a header on the forwarded path too means ts_free never has to
 * guess whose pointer it got, and a pointer we didn't allocate is caught by
 * the magic check instead of being passed to the wrong free.
 */
#include "tsmalloc.h"

#include "internal.h"
#include "os.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TS_MAGIC_LARGE 0x4c47ae11u /* region from ts_os_alloc */
#define TS_MAGIC_LIBC  0x11bcae11u /* small block forwarded to libc */

typedef struct {
    size_t   usable; /* bytes the caller may use, starting at the user pointer */
    uint32_t offset; /* user pointer - start of the underlying allocation */
    uint32_t magic;
} ts_header_t;

_Static_assert(sizeof(ts_header_t) == TS_ALIGN, "header keeps user pointers aligned");

TS_INLINE ts_header_t *header_of(const void *p) {
    return (ts_header_t *)((char *)p - sizeof(ts_header_t));
}

static void fatal(const char *msg) {
    /* write(2) rather than stdio: stdio may allocate. */
    (void)!write(STDERR_FILENO, msg, strlen(msg));
    abort();
}

/* Allocate `size` bytes aligned to `align` (a power of two >= TS_ALIGN).
 * The extra `align - TS_ALIGN` bytes leave room to slide the user pointer up
 * to the alignment; at TS_ALIGN no sliding is needed. */
static void *alloc_aligned(size_t size, size_t align) {
    size_t slack = sizeof(ts_header_t) + (align - TS_ALIGN);
    if (size > SIZE_MAX - slack - TS_ALIGN || align > UINT32_MAX) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = size + slack;

    char *raw;
    uint32_t magic;
    if (size > TS_SMALL_MAX) {
        raw = ts_os_alloc(total);
        magic = TS_MAGIC_LARGE;
    } else {
        raw = malloc(total); /* 16-aligned on every platform we support */
        magic = TS_MAGIC_LIBC;
    }
    if (raw == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    char *user = (char *)ts_align_up((uintptr_t)raw + sizeof(ts_header_t), align);
    ts_header_t *h = header_of(user);
    h->offset = (uint32_t)(user - raw);
    h->magic = magic;
    if (magic == TS_MAGIC_LARGE) {
        /* The mapping is rounded up to whole OS pages; the tail is ours to
         * hand out, which is what makes realloc-in-place possible later. */
        size_t mapped = ts_align_up(total, ts_os_page_size());
        h->usable = mapped - h->offset;
    } else {
        h->usable = total - h->offset;
    }
    return user;
}

static ts_header_t *checked_header(const void *p) {
    ts_header_t *h = header_of(p);
    if (ts_unlikely(h->magic != TS_MAGIC_LARGE && h->magic != TS_MAGIC_LIBC))
        fatal("tsmalloc: free/realloc of a pointer not from tsmalloc (or already freed)\n");
    return h;
}

void *ts_malloc(size_t size) {
    return alloc_aligned(size, TS_ALIGN);
}

void ts_free(void *p) {
    if (p == NULL)
        return;
    ts_header_t *h = checked_header(p);
    char *raw = (char *)p - h->offset;
    uint32_t magic = h->magic;
    h->magic = 0; /* a second free of the same pointer now hits the check */
    if (magic == TS_MAGIC_LARGE)
        ts_os_free(raw, h->offset + h->usable);
    else
        free(raw);
}

size_t ts_usable_size(const void *p) {
    if (p == NULL)
        return 0;
    return checked_header(p)->usable;
}

void *ts_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = ts_malloc(total);
    if (p == NULL)
        return NULL;
    /* Fresh mappings are already zero. */
    if (header_of(p)->magic != TS_MAGIC_LARGE)
        memset(p, 0, total);
    return p;
}

void *ts_realloc(void *p, size_t size) {
    if (p == NULL)
        return ts_malloc(size);
    size_t old = ts_usable_size(p);
    /* Fits already and wouldn't waste more than half: keep the block. */
    if (size <= old && size >= old / 2)
        return p;
    void *q = ts_malloc(size);
    if (q == NULL)
        return NULL; /* p stays valid, as the standard requires */
    memcpy(q, p, old < size ? old : size);
    ts_free(p);
    return q;
}

int ts_posix_memalign(void **out, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || !ts_is_pow2(alignment))
        return EINVAL;
    void *p = alloc_aligned(size, alignment < TS_ALIGN ? TS_ALIGN : alignment);
    if (p == NULL)
        return ENOMEM;
    *out = p;
    return 0;
}

void *ts_aligned_alloc(size_t alignment, size_t size) {
    void *p = NULL;
    int err = ts_posix_memalign(&p, alignment, size);
    if (err != 0) {
        errno = err;
        return NULL;
    }
    return p;
}
