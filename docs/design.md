# tsmalloc design

This is how tsmalloc is put together and why. The README has the short version; this one follows a
block from `malloc` to `free`, from either thread, and covers what happens to memory when threads
come and go. File names point at where each piece lives.

The design follows [mimalloc](https://github.com/microsoft/mimalloc) (Leijen, Zorn and de Moura,
"Mimalloc: Free List Sharding in Action", 2019). The parts that make it fast are all here. The extras
are left out (secure mode, the arena layer, per-page encoded free lists, `heap_t` objects users can
create), so the whole allocator is about 3,000 lines.

## 1. The shape of memory

```
 segment: 4 MiB, 4 MiB-aligned (one mmap)
 +--------+---------------+-----------+-----------+-----+-----------+
 | header | page 0 data   |  page 1   |  page 2   | ... |  page 63  |
 +--------+---------------+-----------+-----------+-----+-----------+
 ^ p & ~(4 MiB - 1)       ^ 64 KiB boundaries: (p - seg) >> 16 is the page index

 page (64 KiB, or a span of up to 16 pages): blocks of one size class
 +-------+-------+-------+-------+-------+- - - - - - - - - - - - - -+
 | block | block | block | block | block |  not carved yet (bump)   |
 +-------+-------+-------+-------+-------+- - - - - - - - - - - - - -+
```

**Segments** (`src/segment.c`, `segment.h`) are 4 MiB regions aligned to 4 MiB. The alignment is the
trick the whole allocator rests on: for any block, `p & ~(4 MiB - 1)` is the start of its segment,
so the metadata is found by masking, with no lookup table and no header in front of the block. The
segment header holds one descriptor per 64 KiB page (`ts_page_t`), the owning thread's id, the heap
pointer, and the remote-free lists (section 4). Page 0's data starts at 4 KiB, past the header, so every
page starts on a 4 KiB boundary. That is what lets `aligned_alloc` with small alignments come from
ordinary size classes.

**Pages** hold blocks of a single size. The page descriptor lives in the segment header, not in the
page, so the page itself is nothing but blocks. Finding a block's page is a subtraction and a shift.
A fresh page isn't threaded into a free list all at once, which would write to (and fault in) all
64 KiB. A bump cursor (`reserved`) instead links 4 KiB worth of blocks at a time, when the free list
runs dry (`src/page.c`).

**Size classes** (`src/size_class.c`): 16-byte steps up to 128 B, then four classes per power of two,
so no more than 25% of a block is wasted. Up to 16 KiB a class gets single pages. From 16 KiB to
512 KiB, "medium" classes get **spans**: runs of up to 16 contiguous pages that act as one page
(the first descriptor does the work, the rest are marked `TAIL` and `span_offset` points back to the
head).

**Huge** blocks above 512 KiB get an mmap of their own, also aligned to 4 MiB, with a 16-byte
`ts_huge_t` header at the start (`src/huge.c`). The first word of a segment and of a huge mapping is
a magic `kind`, so `free` can tell them apart after the same mask. Freed huge mappings go into a
small cache, bucketed by size and capped at 128 MiB, because mmap, munmap and the page faults that
follow are the expensive part of a big allocation.

## 2. Heaps: one per thread

A heap (`src/heap.h`) is what a thread allocates from. It has a queue of pages for each size class, its
list of segments, a spare empty page per class, and a cache of up to two empty segments. Each thread
gets its own heap on its first `malloc`, and nobody else allocates from it. So the allocation path
needs no lock and no atomic instruction. All the synchronization in tsmalloc is on the
paths where a block crosses threads.

The heap pointer is kept in thread-local storage. On macOS, `_Thread_local` can itself call `malloc`
the first time a thread touches it (dyld's lazy TLV setup). That is a problem once tsmalloc *is*
malloc. So there the heap sits in a pthread key's slot, read straight from the thread's TSD array.
The key's destructor is also what notices a thread exiting (section 5).

## 3. malloc and free on the owning thread

The malloc fast path (`small_alloc` in `src/tsmalloc.c`) for requests up to 1 KiB is:

```c
page = heap->pages_direct[(size + 15) >> 4];  // head of that class's queue
b = page->free;                               // pop
page->free = b->next;
page->used++;
```

`pages_direct` maps every 16-byte size step straight to a page, so there is no class lookup and no
queue walk. An empty queue points at a shared, permanently empty page rather than NULL, which saves
a branch. Anything else (no heap yet, the page ran dry, a bigger size) goes to an out-of-line slow
path.

`free` masks the pointer to find the segment and compares the segment's `thread_id` with the
calling thread's id (read from a register-relative TLS slot, with no call). If they match, the block
is pushed onto the page's **`local_free`** list and, unless the page just became empty or was off
its queue, that's the end. No atomics on this path either.

Why two lists, `free` and `local_free`, instead of one? If the owner's frees went straight back on
`free`, a thread that allocates and frees at the same rate would never see `free` run dry. It would
never take the slow path, and that is where remote frees get collected and purging is ticked. With
the owner's frees parked on `local_free`, `free` empties regularly. The slow path then collects
everything in one go: `local_free` becomes the new `free`, and remote frees are spliced in front.
This is mimalloc's "free list sharding".

## 4. Freeing a block from another thread

Every page has a third list, **`thread_free`**, and it is the only part of a page that other threads
write. These lists sit in an array at the end of the segment header, on their own cache lines, away
from the descriptors. A remote free then doesn't bounce the line holding the owner's `free` pointer
and `used` count in the middle of a malloc (step 29 measured this in `cache_scratch` and `larson`).

A remote free is one CAS loop (`ts_heap_free_remote` in `src/heap.c`):

```c
old = load(tf, relaxed);
do {
    b->next = old;
} while (!cas_weak(tf, &old, b, release, relaxed));
```

The owner takes the whole list on its slow path with one atomic instruction (`ts_page_collect` in
`src/page.c`):

```c
head = fetch_and(tf, FLAGS, acquire);   // take the list, keep the flag bits
```

### Why these memory orderings are enough

- The freer writes `b->next` with a plain store, then publishes `b` with a **release** CAS. The owner
  takes the list with an **acquire** RMW. Release/acquire on the same atomic means everything the
  freer wrote before its CAS is visible to the owner after the exchange. That covers `b->next` and
  also the user's last writes into the block. So the owner can walk the list and hand the block to
  someone else safely.
- Each freer's CAS is a read-modify-write, so the pushes form one chain in the variable's
  modification order. Each CAS read the value the previous one wrote. The release sequence rule says
  the owner's acquire synchronizes with *every* earlier releasing push in that chain, not only the
  last one. So all the `next` links on the list are visible, however many threads pushed.
- **ABA can't happen.** ABA needs someone to pop a single node and push it back while another thread's
  CAS is in flight. Here, only the owner removes anything, and it always takes the whole list.
  A freer's CAS succeeds only if the head is the exact value it read, and then the list it links to
  is the list that was there. The block it is pushing was handed to it by the program, so nothing
  else holds it.
- After its CAS succeeds the freer must **not touch the page again**: the owner may collect the
  block, empty the page, and unmap the segment right away. The code is arranged so nothing after the
  CAS dereferences the block or the page.
- The fast-path `thread_id` check is a **relaxed** load. Only the owning thread ever stores its own id
  into a segment. So a thread can't read "mine" for a segment it doesn't own, even through a stale
  value: the worst a stale read gives is "not mine", and a block taking the remote path is always
  correct.

### Pages the owner has forgotten

A full page is taken off its heap's queue, so the owner stops looking at its `thread_free`. If remote
frees then land on it, nothing would ever collect them. Before dropping a full page, the owner
CASes its `thread_free` from empty to a `FULL` flag in the low bits (blocks are 16-aligned, so the
low bits are free). The CAS fails if a remote free slipped in, and then the page isn't full any more.
The first remote freer to see `FULL` swaps it for `BUSY`. It pushes its block onto the heap's
`delayed_free` list (another lock-free stack, emptied by the owner on its slow path), then clears
`BUSY`. When the owner frees that block, it re-queues the page. Later freers push onto the page as
normal. While `BUSY` is set, the owner won't free the delayed blocks of that page or give up the heap,
so the freer may still touch both. The release in `ts_page_mark_full` pairs with the freer's acquire
on winning `FULL`, so the freer's read of `seg->heap` sees the heap that set the flag.

## 5. Threads that exit: abandonment and adoption

When a thread exits, the pthread key destructor calls `ts_heap_abandon`. Segments still holding live
blocks can't be freed, and blocks in them will keep being freed by other threads. The order matters:

1. Clear `FULL` on every page, waiting out any freer that is `BUSY`. After this no freer can reach the
   heap through `delayed_free`.
2. Drain `delayed_free`.
3. Collect each page's `thread_free`, give back empty pages and segments, set each remaining
   segment's owner to 0, and push it onto a global abandoned list (under a mutex).

Frees into an abandoned segment see owner 0, which never matches a live thread, so they take the
remote path and pile up on the pages' `thread_free` lists. The next heap that is about to map a
fresh segment first pops one off the abandoned list (`ts_heap_adopt`), collects what was freed into it,
and queues its pages as its own. Popping under the mutex is what makes the segment ours, so no CAS
on the owner field is needed. The mutex also orders everything the exiting thread wrote before the
adopter reads it.

Thread ids (the `pthread_self` value) are reused after a thread exits. This is why abandoned segments
must drop to owner 0: a new thread with the same id would otherwise free into them as if they were
its own.

## 6. Giving memory back

- **Empty pages** go back to their segment, but each heap keeps one empty page per class queued as a
  spare. A program that frees the last block of a page and immediately allocates again doesn't pay
  to retire and re-initialize the page.
- **Empty segments** go back to the OS, except for up to two per heap.
- **Purging** (`ts_heap_tick`): a page that has stayed free for a whole sweep period is decommitted
  with `madvise(MADV_FREE)` (falling back to `MADV_DONTNEED`). The memory stays mapped but the OS can
  reclaim it. The clock counts slow-path events, not time, so an idle heap does no work.
  `TSMALLOC_PURGE_DELAY` changes the period.

## 7. Replacing the system malloc

- **Linux** (`src/override_linux.c`): `libtsmalloc.so` defines `malloc`, `free` and the rest under
  their real names, so `LD_PRELOAD` puts them in front of glibc's. A static override library does the
  same at link time.
- **macOS** (`src/override_macos.c`): `__DATA,__interpose` entries redirect calls from every image,
  and a `malloc_zone_t` registered as the default zone catches code that goes through zones directly.
  Load it with `DYLD_INSERT_LIBRARIES`. SIP strips that variable for binaries in `/bin` and `/usr/bin`.
- **Foreign pointers.** An injected allocator gets `free` calls for memory allocated before it
  loaded. Masking such a pointer and reading a "header" could fault. So every 4 MiB chunk we own is
  marked in a bitmap over the 48-bit address space (`src/registry.c`): 8 MiB reserved, almost none of
  it ever touched. `ts_is_ours` checks the bit and the header. A huge mapping reserves the rest of its
  last chunk, so a chunk with its bit set holds nobody else's memory, and the header read can't race
  with us unmapping a chunk someone else's pointer lives in. On macOS foreign pointers go back to
  the zone that allocated them. On Linux `free` ignores them and `realloc` fails on them.
- **fork**: `pthread_atfork` handlers hold the global locks (abandoned list, huge cache, the
  `busy_gate` around the delayed path) across `fork`, so the child doesn't inherit a lock held by a
  thread that no longer exists.

## 8. How the system allocators do it

**glibc ptmalloc** splits memory into *arenas*, each a heap behind one mutex. A thread picks an arena
and sticks to it, and new arenas are created under contention (up to 8 per core on 64-bit). `free`
locks the arena that owns the chunk, so a remote free contends with the owning arena's allocations.
Chunks carry an 8–16 byte header (size, and prev-size boundary tags) so neighbours can be coalesced.
Since glibc 2.26 a per-thread **tcache** holds a few chunks per size with no lock, which makes
same-thread malloc/free pairs fast. Producer/consumer patterns still pay for the lock and the
header cache misses. That is where tsmalloc's lock-free `thread_free` push wins most (`xmalloc`, 4–5x).

**macOS libmalloc** (the "scalable zone") uses *magazines*: one per CPU, each with a lock, chosen by
the CPU the thread is running on at the time. Tiny (≤ 1 KiB) and small blocks come from regions owned by a
magazine, and a block is freed back to its region's owner, under that magazine's lock. A depot
magazine collects empty regions for reuse. Per-CPU magazines keep lock contention low, but every
operation still takes a lock, and a thread that migrates between CPUs touches another magazine's
cache lines. tsmalloc's per-thread heaps avoid both: the owning thread never locks, and a remote free
is one CAS on a line only freers write.

**What tsmalloc gives up.** No coalescing: memory in a page can only be reused for its own size class
until the whole page is empty. A program that allocates many blocks of one size, frees most of them
and switches to another size holds more memory than ptmalloc would. Per-thread heaps also mean each
thread keeps partly used pages of its own, so many threads with small, scattered working sets
use more memory than a few shared arenas. These are the usual costs of this design. mimalloc has
the same ones, and limits them with the same tools: purging, spare-page caps and adoption.

Measurements are in [benchmarks.md](benchmarks.md).
