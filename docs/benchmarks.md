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

Next comes thread-local heaps (step 18 onward), which take the lock out of the common path entirely.
