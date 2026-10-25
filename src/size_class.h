#ifndef TS_SIZE_CLASS_H
#define TS_SIZE_CLASS_H

#include "internal.h"

#define TS_NUM_CLASSES 36
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

/* Smallest class whose block size is >= size. size must be <= TS_SMALL_MAX;
 * size 0 maps to the 16 B class. */
TS_INLINE unsigned ts_size_to_class(size_t size) {
    if (ts_likely(size <= TS_LUT_MAX))
        return ts_class_lut[(size + 15) >> 4];
    return ts_size_to_class_large(size);
}

TS_INLINE size_t ts_class_size(unsigned cls) {
    return ts_class_sizes[cls];
}

/* Smallest class that holds `size` bytes and whose block size is a multiple
 * of `align` (a power of two), or TS_NUM_CLASSES if none does. Blocks of
 * such a class are `align`-aligned as long as their page start is, which
 * holds for align <= TS_PAGE0_OFFSET. */
TS_INLINE unsigned ts_size_to_class_aligned(size_t size, size_t align) {
    if (size > TS_SMALL_MAX)
        return TS_NUM_CLASSES;
    unsigned cls = ts_size_to_class(size);
    while (cls < TS_NUM_CLASSES && (ts_class_sizes[cls] & (align - 1)) != 0)
        cls++;
    return cls;
}

#endif /* TS_SIZE_CLASS_H */
