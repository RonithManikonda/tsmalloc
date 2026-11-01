#ifndef TS_SIZE_CLASS_H
#define TS_SIZE_CLASS_H

#include "internal.h"

#define TS_NUM_SMALL_CLASSES 36 /* classes up to TS_SMALL_MAX */
#define TS_NUM_CLASSES 56       /* ... and the medium ones up to TS_MEDIUM_MAX */
#define TS_LUT_MAX     ((size_t)1024)

extern const uint32_t ts_class_sizes[TS_NUM_CLASSES];
extern const uint8_t ts_class_lut[TS_LUT_MAX / 16 + 1];

/* Class for sizes above 128 B, by bit math. With w = size - 1 and b its top
 * bit, size lies in (2^b, 2^(b+1)]; the two bits below b say which quarter
 * of that range it falls in. */
TS_INLINE unsigned ts_size_to_class_large(size_t size) {
    size_t w = size - 1;
    unsigned b = ts_bsr(w);
    unsigned quarter = (unsigned)(w >> (b - 2)) & 3;
    return 8 + (b - 7) * 4 + quarter;
}

/* Smallest class whose block size is >= size. size must be <= TS_MEDIUM_MAX;
 * size 0 maps to the 16 B class. */
TS_INLINE unsigned ts_size_to_class(size_t size) {
    if (ts_likely(size <= TS_LUT_MAX))
        return ts_class_lut[(size + 15) >> 4];
    return ts_size_to_class_large(size);
}

TS_INLINE size_t ts_class_size(unsigned cls) {
    return ts_class_sizes[cls];
}

/* Pages a page (span) of class `cls` covers: 1 for small classes; for
 * medium ones enough for about 8 blocks, at most TS_SPAN_MAX_PAGES. Spans
 * are reserved, not touched: a block's memory is only written when it is
 * handed out, so a big span with one block in use costs one block's worth
 * of RAM. */
TS_INLINE size_t ts_class_span_pages(unsigned cls) {
    size_t bsize = ts_class_sizes[cls];
    if (bsize <= TS_SMALL_MAX)
        return 1;
    size_t pages = (8 * bsize + TS_PAGE_SIZE - 1) >> TS_PAGE_SHIFT;
    return pages < TS_SPAN_MAX_PAGES ? pages : TS_SPAN_MAX_PAGES;
}

/* Smallest class that holds `size` bytes and whose block size is a multiple
 * of `align` (a power of two), or TS_NUM_CLASSES if none does. Blocks of
 * such a class are `align`-aligned as long as their page start is, which
 * holds for align <= TS_PAGE0_OFFSET (spans start on page boundaries too). */
TS_INLINE unsigned ts_size_to_class_aligned(size_t size, size_t align) {
    if (size > TS_MEDIUM_MAX)
        return TS_NUM_CLASSES;
    unsigned cls = ts_size_to_class(size);
    while (cls < TS_NUM_CLASSES && (ts_class_sizes[cls] & (align - 1)) != 0)
        cls++;
    return cls;
}

#endif /* TS_SIZE_CLASS_H */
