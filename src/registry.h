#ifndef TS_REGISTRY_H
#define TS_REGISTRY_H

#include "internal.h"

/*
 * Which 4 MiB chunks of the address space hold our regions: one bit per
 * chunk, in an 8 MiB table mapped the first time it is needed. Almost all
 * of the table is never touched, so it costs a few OS pages in practice.
 *
 * When tsmalloc replaces malloc, free can be handed pointers from whoever
 * allocated before us. Masking such a pointer and reading a header there
 * could fault, so the bit has to be checked first.
 */

/* Addresses at or above this aren't tracked; they are never ours. */
#define TS_REGISTRY_ADDRESS_BITS 48

/* Mark the chunk starting at `region` (segment-size aligned) as ours. Returns
 * 0 if the table can't be mapped; the caller should give up the region. */
int ts_registry_add(const void *region);

/* Forget a chunk. Call before the region is unmapped or goes to a cache. */
void ts_registry_remove(const void *region);

/* Is `p` a pointer this allocator handed out and hasn't taken back? Intended
 * for the start of a block or a foreign pointer. A pointer inside a segment
 * counts as ours whichever block it is in, or whether that block is live. */
int ts_is_ours(const void *p);

#endif /* TS_REGISTRY_H */
