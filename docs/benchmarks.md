# Benchmarks

Numbers come from `scripts/bench.sh`: a Release build, each benchmark at 1, 2, 4, 8 and 14 threads,
3 runs per cell, median shown. Throughput is millions of operations per second (Mops/s), so higher is
better. The last column is tsmalloc divided by the system allocator: above 1.00x means tsmalloc wins.

| Benchmark | What it does |
|---|---|
| `threadtest-N` | every thread mallocs a batch of N-byte blocks, then frees them; nothing is shared |
| `larson` | server simulation: threads replace random blocks (16–1024 B), then hand their arrays to new threads |
| `xmalloc` | producer/consumer pairs: one thread allocates, the other frees (every free is remote) |
| `cache_scratch` | main allocates small objects, workers write to them; measures false sharing, ops are byte writes |

## Thread-local heaps (steps 18–24)

Same machine and script, macOS 26.3.1. Each thread allocates from its own heap without locks.
Blocks freed by another thread go onto their page's lock-free `thread_free` list. Heaps of exited
threads are abandoned and their segments adopted by others. Empty pages and segments are kept with
some hysteresis. The whole sweep now takes about 10 seconds; with the global lock it took 18 minutes.

| bench | threads | system Mops/s | ts Mops/s | ts / system |
|---|---:|---:|---:|---:|
| cache_scratch | 1 | 3499.18 | 3549.01 | 1.01x |
| cache_scratch | 2 | 2865.46 | 8206.81 | 2.86x |
| cache_scratch | 4 | 3821.31 | 16827.49 | 4.40x |
| cache_scratch | 8 | 9364.67 | 31458.13 | 3.36x |
| cache_scratch | 14 | 14419.80 | 39373.54 | 2.73x |
| larson | 1 | 22.26 | 122.07 | 5.48x |
| larson | 2 | 21.66 | 240.56 | 11.11x |
| larson | 4 | 23.91 | 419.64 | 17.55x |
| larson | 8 | 32.79 | 473.32 | 14.43x |
| larson | 14 | 41.92 | 476.58 | 11.37x |
| threadtest-1024 | 1 | 47.05 | 229.36 | 4.87x |
| threadtest-1024 | 2 | 21.59 | 447.83 | 20.74x |
| threadtest-1024 | 4 | 47.85 | 818.33 | 17.10x |
| threadtest-1024 | 8 | 37.28 | 1440.40 | 38.64x |
| threadtest-1024 | 14 | 18.56 | 1613.65 | 86.94x |
| threadtest-16 | 1 | 60.53 | 270.12 | 4.46x |
| threadtest-16 | 2 | 151.72 | 536.48 | 3.54x |
| threadtest-16 | 4 | 150.93 | 944.29 | 6.26x |
| threadtest-16 | 8 | 281.35 | 1815.71 | 6.45x |
| threadtest-16 | 14 | 271.02 | 1872.16 | 6.91x |
| threadtest-256 | 1 | 63.34 | 266.67 | 4.21x |
| threadtest-256 | 2 | 28.02 | 520.02 | 18.56x |
| threadtest-256 | 4 | 88.59 | 948.32 | 10.70x |
| threadtest-256 | 8 | 92.70 | 1809.14 | 19.52x |
| threadtest-256 | 14 | 100.54 | 1788.45 | 17.79x |
| threadtest-64 | 1 | 56.72 | 264.69 | 4.67x |
| threadtest-64 | 2 | 131.08 | 528.82 | 4.03x |
| threadtest-64 | 4 | 203.87 | 944.29 | 4.63x |
| threadtest-64 | 8 | 140.68 | 1820.66 | 12.94x |
| threadtest-64 | 14 | 191.24 | 1820.07 | 9.52x |
| xmalloc | 2 | 21.09 | 34.86 | 1.65x |
| xmalloc | 4 | 23.83 | 77.99 | 3.27x |
| xmalloc | 8 | 24.33 | 83.07 | 3.41x |
| xmalloc | 14 | 29.54 | 124.80 | 4.22x |

tsmalloc now wins every row, and it scales with the thread count where libmalloc mostly doesn't.

- **threadtest** (no sharing) is the best case for a thread-local design: malloc is a pop and free a
  push on the thread's own page, with no atomics at all. One thread is 4–5x libmalloc, and from 1 to
  14 threads tsmalloc goes from ~265 to ~1800 Mops/s, about 7x. The very large ratios (87x for
  `threadtest-1024` at 14 threads) say more about libmalloc having a bad time with that size than
  about tsmalloc. Look at the absolute numbers rather than those outliers.
- **larson** is 5–18x. Its blocks are freed by the thread that inherits the array, so a good share of
  frees are remote; the CAS push onto `thread_free` plus the owner's batch collect handles that
  without any lock. Throughput flattens past 8 threads. That's the next thing to look at: the
  efficiency cores, and pages that bounce between threads.
- **xmalloc** is the hardest case: every free is remote. tsmalloc still wins 1.7–4.2x, but in
  absolute terms it is the slowest benchmark (35–125 Mops/s). Each block crosses cores twice, and
  the producer depends on collecting its pages back from the consumers. That's the target for the
  free-path and cache-line work in phase 5.
- **cache_scratch** flipped from 0.4x to 2.7–4.4x. Each worker frees its object and allocates a new
  one from its own heap, so the objects it writes to no longer share cache lines with other workers'.
  That's passive false sharing going away, the thing this benchmark exists to show.

Caveats: these are microbenchmarks on a laptop with performance and efficiency cores, the system
numbers jump around between thread counts (see the baseline notes below), and tsmalloc doesn't yet
return idle memory to the OS (purging is step 32), so it isn't paying costs that libmalloc pays.
Linux/glibc numbers will come from CI.

## Baseline: one global heap behind one mutex (step 11)

Machine: Apple M4 Pro (14 cores), macOS 26.3, Apple clang 21. System allocator: macOS libmalloc.

| bench | threads | system Mops/s | ts Mops/s | ts / system |
|---|---:|---:|---:|---:|
| cache_scratch | 1 | 1979.14 | 1935.45 | 0.98x |
| cache_scratch | 2 | 1246.52 | 1453.10 | 1.17x |
| cache_scratch | 4 | 2922.27 | 3333.45 | 1.14x |
| cache_scratch | 8 | 7932.67 | 6185.22 | 0.78x |
| cache_scratch | 14 | 14076.72 | 5796.07 | 0.41x |
| larson | 1 | 12.10 | 30.50 | 2.52x |
| larson | 2 | 9.89 | 14.48 | 1.46x |
| larson | 4 | 11.33 | 1.99 | 0.18x |
| larson | 8 | 17.83 | 3.71 | 0.21x |
| larson | 14 | 18.43 | 3.64 | 0.20x |
| threadtest-1024 | 1 | 53.34 | 116.80 | 2.19x |
| threadtest-1024 | 2 | 21.84 | 57.88 | 2.65x |
| threadtest-1024 | 4 | 50.79 | 15.63 | 0.31x |
| threadtest-1024 | 8 | 36.36 | 11.08 | 0.30x |
| threadtest-1024 | 14 | 19.22 | 10.68 | 0.56x |
| threadtest-16 | 1 | 81.58 | 125.13 | 1.53x |
| threadtest-16 | 2 | 50.97 | 38.39 | 0.75x |
| threadtest-16 | 4 | 91.74 | 13.92 | 0.15x |
| threadtest-16 | 8 | 181.40 | 11.85 | 0.07x |
| threadtest-16 | 14 | 223.75 | 11.50 | 0.05x |
| threadtest-256 | 1 | 73.38 | 127.32 | 1.74x |
| threadtest-256 | 2 | 28.72 | 51.15 | 1.78x |
| threadtest-256 | 4 | 88.21 | 15.59 | 0.18x |
| threadtest-256 | 8 | 87.54 | 11.08 | 0.13x |
| threadtest-256 | 14 | 94.33 | 8.50 | 0.09x |
| threadtest-64 | 1 | 76.24 | 125.63 | 1.65x |
| threadtest-64 | 2 | 119.59 | 52.35 | 0.44x |
| threadtest-64 | 4 | 78.49 | 15.25 | 0.19x |
| threadtest-64 | 8 | 137.87 | 10.23 | 0.07x |
| threadtest-64 | 14 | 189.64 | 12.18 | 0.06x |
| xmalloc | 2 | 14.39 | 13.00 | 0.90x |
| xmalloc | 4 | 10.21 | 2.55 | 0.25x |
| xmalloc | 8 | 16.21 | 1.70 | 0.10x |
| xmalloc | 14 | 24.41 | 5.24 | 0.21x |

This version loses badly, and that's the expected result. It's here to be beaten.

- **One thread:** tsmalloc wins by 1.5–2.5x. With no contention the lock is cheap, and a pop from a
  per-class free list is less work than libmalloc's general path.
- **Two or more threads:** it falls apart. At 4 threads and up, every small `malloc` and `free` in
  the process queues on the same mutex. `threadtest-16` at 14 threads runs at 5% of libmalloc's
  speed, and adding threads makes tsmalloc *slower* in absolute terms while libmalloc keeps scaling.
  The sweep spends most of its time in the kernel (`sys` was about twice `user`): contended threads
  sleep and wake on the lock instead of allocating.
- **xmalloc and larson**, the cross-thread workloads, land at 0.1–0.25x. Here the lock isn't even
  the only problem: blocks bounce between cores along with the heap metadata they touch.
- **cache_scratch** is nearly a tie until 8 threads. It allocates once up front, so it mostly
  measures where objects are placed, not allocator speed. Falling behind at 14 threads probably means
  neighbouring small objects share cache lines across workers. This benchmark is noisy (±30%).

The system numbers jump around between thread counts (e.g. `threadtest-64`: 120 Mops/s at 2 threads,
78 at 4). libmalloc's magazines get assigned to cores as threads come and go, and on a laptop the
performance and efficiency cores differ. Treat single cells as rough. The overall trend is what holds.

That was the starting point for thread-local heaps (step 18 onward), which take the lock out of the
common path entirely.
