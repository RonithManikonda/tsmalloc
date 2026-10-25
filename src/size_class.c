#include "size_class.h"

/* Block size of each class. Classes 0-7 are 16 B apart (16..128); after that
 * every power-of-two range (128, 256], (256, 512], ... is split into four
 * equal steps, so a request never wastes more than 25% of its size above
 * 128 B. */
const uint32_t ts_class_sizes[TS_NUM_CLASSES] = {
    16, 32, 48, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 896, 1024, 1280, 1536, 1792, 2048,
    2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192,
    10240, 12288, 14336, 16384,
};

/* ts_class_lut[(size + 15) / 16] = class for sizes up to TS_LUT_MAX.
 * One byte per 16 B of request size keeps the whole table in one or two
 * cache lines. test_size_class checks it against the bit-math formula. */
const uint8_t ts_class_lut[TS_LUT_MAX / 16 + 1] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11,
    11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15,
    15, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17,
    17, 18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19,
    19,
};
