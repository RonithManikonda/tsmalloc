#ifndef TS_INTERNAL_H
#define TS_INTERNAL_H

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Layout constants ------------------------------------------------------
 *
 * segment (4 MiB, 4 MiB aligned)  ->  64 pages of 64 KiB  ->  blocks of one size
 *
 * Because segments are aligned to their size, the segment holding any small
 * pointer is `p & ~TS_SEGMENT_MASK`, and its page index is
 * `(p - segment) >> TS_PAGE_SHIFT`.
 */
#define TS_SEGMENT_SHIFT 22
#define TS_SEGMENT_SIZE  ((size_t)1 << TS_SEGMENT_SHIFT)
#define TS_SEGMENT_MASK  (TS_SEGMENT_SIZE - 1)

#define TS_PAGE_SHIFT 16
#define TS_PAGE_SIZE  ((size_t)1 << TS_PAGE_SHIFT)
#define TS_PAGES_PER_SEGMENT (TS_SEGMENT_SIZE / TS_PAGE_SIZE)

/* Largest size served from pages; anything above goes to the large path. */
#define TS_SMALL_MAX ((size_t)16 * 1024)

/* Every block is at least this aligned (what malloc must guarantee). */
#define TS_ALIGN ((size_t)16)

_Static_assert(sizeof(void *) == 8, "tsmalloc assumes a 64-bit address space");
_Static_assert(TS_PAGE_SHIFT < TS_SEGMENT_SHIFT, "pages must fit in a segment");
_Static_assert(TS_SEGMENT_SIZE % TS_PAGE_SIZE == 0, "segment is whole pages");
_Static_assert((TS_ALIGN & (TS_ALIGN - 1)) == 0, "TS_ALIGN is a power of two");
_Static_assert(TS_ALIGN >= alignof(max_align_t), "TS_ALIGN covers max_align_t");
_Static_assert(TS_SMALL_MAX % TS_ALIGN == 0, "small sizes stay aligned");
/* At least 4 blocks of the biggest small class per page, or a page is mostly
 * metadata overhead and the per-page free lists stop paying off. */
_Static_assert(TS_SMALL_MAX * 4 <= TS_PAGE_SIZE, "small blocks pack into pages");

/* ---- Compiler helpers ------------------------------------------------------ */
#define ts_likely(x)   __builtin_expect(!!(x), 1)
#define ts_unlikely(x) __builtin_expect(!!(x), 0)

#define TS_INLINE   static inline __attribute__((always_inline))
#define TS_NOINLINE __attribute__((noinline))

/* `align` must be a power of two. Callers that might overflow check first. */
TS_INLINE size_t ts_align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

TS_INLINE size_t ts_align_down(size_t n, size_t align) {
    return n & ~(align - 1);
}

TS_INLINE int ts_is_pow2(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

/* Index of the highest set bit; x must be non-zero. ts_bsr(1) == 0. */
TS_INLINE unsigned ts_bsr(size_t x) {
    return (unsigned)(63 - __builtin_clzll((unsigned long long)x));
}

/* ---- Thread identity -------------------------------------------------------
 *
 * A cheap id for the calling thread, unique among live threads and never 0,
 * so 0 can mean "no owner". Where we know the layout we read the thread's
 * own self pointer straight out of its TLS block (what pthread_self()
 * returns), which is a single load; elsewhere we call pthread_self().
 *
 *   - Darwin arm64: TPIDRRO_EL0 points at the TSD array (low 3 bits hold the
 *     CPU number); slot 0 is the pthread_t.
 *   - glibc x86-64: %fs:0 is the TCB's pointer to itself, i.e. pthread_self().
 *   - Linux arm64: the thread pointer register is per-thread TLS base.
 *
 * Ids get reused once a thread is gone, so anything keyed on them has to
 * forget a thread when it exits. */
#if defined(__APPLE__) && defined(__aarch64__)
TS_INLINE uintptr_t ts_thread_id(void) {
    uintptr_t tsd;
    __asm__("mrs %0, tpidrro_el0" : "=r"(tsd));
    return ((const uintptr_t *)(tsd & ~(uintptr_t)7))[0];
}
#elif defined(__linux__) && defined(__x86_64__)
TS_INLINE uintptr_t ts_thread_id(void) {
    uintptr_t self;
    __asm__("movq %%fs:0, %0" : "=r"(self));
    return self;
}
#elif defined(__linux__) && defined(__aarch64__)
TS_INLINE uintptr_t ts_thread_id(void) {
    return (uintptr_t)__builtin_thread_pointer();
}
#else
#include <pthread.h>
TS_INLINE uintptr_t ts_thread_id(void) {
    return (uintptr_t)pthread_self();
}
#endif

/* ---- Core types (defined as each part is written) -------------------------- */
typedef struct ts_block_s   ts_block_t;
typedef struct ts_page_s    ts_page_t;
typedef struct ts_segment_s ts_segment_t;
typedef struct ts_heap_s    ts_heap_t;

#endif /* TS_INTERNAL_H */
