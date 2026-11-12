# Benchmarks

Numbers come from `scripts/bench.sh` and `scripts/bench_override.sh`: a Release build, each benchmark at
1, 2, 4, 8 and 14 threads, 3 runs per cell, median shown. Throughput is millions of operations per second (Mops/s), so higher is
better. The last column is tsmalloc divided by the system allocator: above 1.00x means tsmalloc wins.

| Benchmark | What it does |
|---|---|
| `threadtest-N` | every thread mallocs a batch of N-byte blocks, then frees them; nothing is shared |
| `larson` | server simulation: threads replace random blocks (16–1024 B), then hand their arrays to new threads |
| `xmalloc` | producer/consumer pairs: one thread allocates, the other frees (every free is remote) |
| `cache_scratch` | main allocates small objects, workers write to them; measures false sharing, ops are byte writes |

## Summary

### Method

- **What runs.** The four programs above, in a Release build (`-O3`, `-pthread`). The headline numbers use
  the `*_plain` builds through `scripts/bench_override.sh`. These programs call only `malloc` and `free` and
  contain no tsmalloc code. Each one runs once as built, and once with `libtsmalloc` injected
  (`DYLD_INSERT_LIBRARIES` on macOS, `LD_PRELOAD` on Linux). So the comparison is what an ordinary
  `-pthread` program gets without changing its source. Every run reports the allocator that actually served
  it, and the script stops if that isn't the one requested.
- **Cells.** 1, 2, 4, 8 and 14 threads (xmalloc rounds up to pairs). The median of 3 runs per cell on the
  Mac, 1 run in CI. Mops/s is millions of operations per second, so higher is better. The last column is
  tsmalloc ÷ system.
- **Noise.** On the Mac, single cells move by ±30% between runs, and everything can halve when the machine
  is busy (check `uptime` first). Treat a cell as rough and the pattern across a row as what holds.

### Machines

| Where | CPU | OS / system allocator | Runs |
|---|---|---|---|
| Development Mac | Apple M4 Pro, 14 cores (10 performance + 4 efficiency), arm64 | macOS 26.3.1, libmalloc | 3 per cell |
| Docker on that Mac | same, 14 virtual CPUs | Ubuntu 24.04 arm64, glibc 2.39 | 3 per cell |
| GitHub Actions `ubuntu-latest` | x86-64, 4 vCPUs | Ubuntu 26.04, glibc | 1, `--quick` |
| GitHub Actions `macos-latest` | Apple silicon VM, 3 vCPUs | macOS 26, libmalloc | 1, `--quick` |

### Headline: unmodified programs, tsmalloc ÷ system

Ranges over thread counts, from the full tables further down: macOS and Docker from step 39, x86-64 from
the CI release job of 2026-11-10 (override runs, `gcc` job). The `clang` job's cells differ from these by up to
2x in both directions, which is how noisy single runs on shared runners are.

| bench | macOS, M4 Pro | Linux arm64 (Docker) | Linux x86-64 (CI) |
|---|---:|---:|---:|
| `threadtest` (4 sizes) | 3.3x – 89x | 1.4x – 2.5x | 1.3x – 2.2x |
| `larson` | 3.1x – 12x | 1.3x – 1.4x | 0.87x – 1.05x |
| `xmalloc` | 1.5x – 3.3x | 4.3x – 5.1x | 5.9x – 6.5x |
| `cache_scratch` | 1.05x – 4.4x | 1.0x – 5.8x | 0.99x – 3.6x |

On macOS tsmalloc is ahead in every cell, by an order of magnitude once several threads allocate at the
same time. On Linux it is ahead nearly everywhere, and by far the most where blocks cross threads.

### Where tsmalloc wins, where it doesn't, and why

- **Threads that allocate and free their own blocks (`threadtest`).** tsmalloc's fast path is a few loads
  and stores on memory only this thread touches, with no lock and no atomic. libmalloc takes a magazine
  lock on every call, and a thread that moves between cores lands on another core's magazine. That is why
  the gap grows with the thread count (89x at 14 threads for 1 KiB blocks). glibc already has a per-thread
  cache (tcache) in front of its arenas, so the gap there is the tcache's bookkeeping and the arena
  refills behind it: 1.4x to 2.5x.
- **Every free from another thread (`xmalloc`).** This is the best case on Linux. In glibc a cross-thread
  free locks the arena that owns the chunk, and producer and consumer contend on that lock all the time.
  In tsmalloc the free is one CAS onto a list only freers write, and the producer takes the whole list
  back with one atomic. On macOS libmalloc gets closer here (1.5x at 2 threads). The cost on tsmalloc's
  side shows in the profile further down: walking a list of blocks last written by another core takes a
  cache miss per block.
- **Blocks handed to new threads (`larson`).** Big wins against libmalloc, but level on x86-64 glibc (0.8x
  to 1.1x across the two CI jobs). larson's threads free many blocks that other threads allocated. glibc puts
  such a chunk in the *freeing* thread's tcache, which reuses it right away with no lock. tsmalloc must
  send it back to the page it came from (a remote free), and the owner sees it only on its next slow path.
  Returning blocks to their owner is what keeps pages whole and memory from scattering across threads,
  so this is a cost the design accepts.
- **False sharing (`cache_scratch`).** At 1 thread every allocator is level, as expected. With more threads
  tsmalloc gives each worker blocks from its own pages, so workers' writes don't share cache lines.
  libmalloc's and glibc's adjacent small blocks can belong to different threads.
- **Huge blocks.** Not in the tables. A loop that allocates, touches and frees 2 MiB runs at about
  2.05 M ops/s with tsmalloc's huge cache against 1.08 M ops/s with libmalloc (step 31).
- **Memory, not speed.** These benchmarks measure throughput only. tsmalloc has no coalescing and keeps
  partly used pages per thread, so a program with many threads and small, scattered working sets can use
  more memory than ptmalloc. Purging and the spare-page caps limit this, but it isn't measured here.

### Caveats

- The Linux arm64 numbers come from Docker on the Mac, and the native x86-64 ones from single `--quick`
  runs on shared CI machines. On the 3-vCPU macOS CI runner one `xmalloc` cell at 2 threads came out at
  0.60x. A dedicated Linux machine is the missing measurement.
- jemalloc and mimalloc aren't compared yet (see the end of this file). Beating the system allocator is
  the goal so far, not beating the specialist allocators.
- Re-runs of the macOS override sweep on 2026-11-11, with the load average between 7 and 11 from other
  work, came out lower across the board. Most of the drop was in the 8-thread cells, where both allocators
  fell. tsmalloc still led in every cell, by similar ratios. Run alternately with a build from before
  steps 41–42, the current build was as fast or faster (`threadtest --threads=8`), so the drop was the
  machine, not a regression.

The rest of this file is the step-by-step record, kept with the numbers as measured at the time.

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

## After fast-path tuning, spans, the huge cache and purging (steps 26–33)

Same machine and script. Compared with the table above, most rows gained 5–20% at higher thread counts
from the fast-path and cache-line work. `larson` at 1 thread reads lower (90 against 122), but that is
machine state rather than a regression: a build from before steps 31–33 and the current one both do about
105 Mops/s when run alternately, and purging on or off makes no measurable difference.

| bench | threads | system Mops/s | ts Mops/s | ts / system |
|---|---:|---:|---:|---:|
| cache_scratch | 1 | 4320.12 | 5030.97 | 1.16x |
| cache_scratch | 2 | 3646.27 | 9861.02 | 2.70x |
| cache_scratch | 4 | 3844.01 | 17542.90 | 4.56x |
| cache_scratch | 8 | 9476.08 | 34275.92 | 3.62x |
| cache_scratch | 14 | 16346.79 | 48837.92 | 2.99x |
| larson | 1 | 22.18 | 90.32 | 4.07x |
| larson | 2 | 24.24 | 175.13 | 7.22x |
| larson | 4 | 24.30 | 314.81 | 12.96x |
| larson | 8 | 34.55 | 435.87 | 12.62x |
| larson | 14 | 48.35 | 528.14 | 10.92x |
| threadtest-1024 | 1 | 48.77 | 245.22 | 5.03x |
| threadtest-1024 | 2 | 21.51 | 490.68 | 22.81x |
| threadtest-1024 | 4 | 47.90 | 874.51 | 18.26x |
| threadtest-1024 | 8 | 33.21 | 1664.59 | 50.12x |
| threadtest-1024 | 14 | 18.91 | 1769.91 | 93.60x |
| threadtest-16 | 1 | 61.46 | 289.52 | 4.71x |
| threadtest-16 | 2 | 53.10 | 570.45 | 10.74x |
| threadtest-16 | 4 | 94.59 | 1005.53 | 10.63x |
| threadtest-16 | 8 | 155.62 | 1933.30 | 12.42x |
| threadtest-16 | 14 | 245.48 | 2161.83 | 8.81x |
| threadtest-256 | 1 | 63.80 | 286.86 | 4.50x |
| threadtest-256 | 2 | 46.42 | 566.57 | 12.21x |
| threadtest-256 | 4 | 88.49 | 1008.06 | 11.39x |
| threadtest-256 | 8 | 95.51 | 1947.42 | 20.39x |
| threadtest-256 | 14 | 100.52 | 2165.84 | 21.55x |
| threadtest-64 | 1 | 60.80 | 286.70 | 4.72x |
| threadtest-64 | 2 | 136.24 | 567.21 | 4.16x |
| threadtest-64 | 4 | 86.75 | 1007.56 | 11.61x |
| threadtest-64 | 8 | 137.54 | 1946.47 | 14.15x |
| threadtest-64 | 14 | 243.85 | 2140.02 | 8.78x |
| xmalloc | 2 | 32.77 | 59.04 | 1.80x |
| xmalloc | 4 | 34.64 | 109.73 | 3.17x |
| xmalloc | 8 | 25.84 | 104.18 | 4.03x |
| xmalloc | 14 | 33.68 | 153.24 | 4.55x |

Sizes above 512 KiB aren't in this table. On a single-thread loop that allocates, touches and frees 2 MiB,
the huge cache does about 2.05 M ops/s, against 14 k ops/s with the cache flushed after each free and
1.08 M ops/s for libmalloc.

## Where the time goes

`sample` on `xmalloc --threads=8` (3 s at 1 ms, stacks from all threads), which is the weakest workload
because every free is remote:

| Top of stack | Samples | What it is |
|---|---:|---|
| `__psynch_cvwait`, `__ulock_wait`, `__psynch_cvsignal` | 10909 | the benchmark's own batch handoff between producer and consumer |
| `ts_page_alloc_slow` | 6969 | taking the remote-free list and walking it to count the blocks |
| `free_slow` + `ts_heap_free_remote` | 1719 | the remote free itself, a CAS push |
| `ts_malloc` | 932 | the fast path |
| `_tlv_get_addr` | 194 | the macOS thread-local lookup on the way to the heap |

- The bench spends more time waiting on its own condition variable than in the allocator.
- `ts_page_alloc_slow` is the real cost. Each block on a remote-free list was last written by another
  core, so walking the list takes a cache miss per block. Those blocks are about to be handed out anyway,
  so the misses move rather than disappear. Having freers keep a count wouldn't remove them, and it would
  add a second contended word to the push.
- `_tlv_get_addr` is small here but it is on every malloc. The override work replaces it with a direct
  thread-specific slot.

Nothing in the profile justified another change to the fast paths, so this step only records the results.

On Linux the equivalent is `perf record -g ./build-rel/bench/xmalloc --alloc=ts --threads=8` followed by
`perf report --no-children`; Linux numbers will come from CI.

## Through the override: unmodified programs (step 39)

This is the comparison the project exists for. `scripts/bench_override.sh` runs the `*_plain` programs, which
contain no tsmalloc and only call `malloc` and `free`. They run once as built and once with tsmalloc injected
(`DYLD_INSERT_LIBRARIES` on macOS, `LD_PRELOAD` on Linux). Each run reports the allocator it actually got, and
the script stops if that is not the one it asked for. 3 runs per cell, median shown, as above.

### macOS 26.3.1, libmalloc (Apple M4 Pro, 14 cores)

| bench | threads | system Mops/s | ts Mops/s | ts / system |
|---|---:|---:|---:|---:|
| cache_scratch | 1 | 4763.04 | 4979.92 | 1.05x |
| cache_scratch | 2 | 3314.83 | 9523.81 | 2.87x |
| cache_scratch | 4 | 3934.10 | 17515.05 | 4.45x |
| cache_scratch | 8 | 8725.53 | 34436.37 | 3.95x |
| cache_scratch | 14 | 18325.51 | 49160.54 | 2.68x |
| larson | 1 | 30.21 | 93.21 | 3.09x |
| larson | 2 | 24.35 | 172.53 | 7.09x |
| larson | 4 | 24.27 | 299.85 | 12.35x |
| larson | 8 | 33.91 | 400.16 | 11.80x |
| larson | 14 | 39.62 | 417.46 | 10.54x |
| threadtest-1024 | 1 | 54.74 | 245.22 | 4.48x |
| threadtest-1024 | 2 | 22.22 | 474.38 | 21.35x |
| threadtest-1024 | 4 | 45.00 | 844.24 | 18.76x |
| threadtest-1024 | 8 | 36.66 | 1624.04 | 44.30x |
| threadtest-1024 | 14 | 19.16 | 1707.73 | 89.13x |
| threadtest-16 | 1 | 83.14 | 279.02 | 3.36x |
| threadtest-16 | 2 | 160.10 | 535.33 | 3.34x |
| threadtest-16 | 4 | 88.92 | 962.46 | 10.82x |
| threadtest-16 | 8 | 200.59 | 1839.08 | 9.17x |
| threadtest-16 | 14 | 256.56 | 2047.38 | 7.98x |
| threadtest-256 | 1 | 75.93 | 273.82 | 3.61x |
| threadtest-256 | 2 | 27.80 | 535.62 | 19.27x |
| threadtest-256 | 4 | 85.28 | 963.86 | 11.30x |
| threadtest-256 | 8 | 93.73 | 1849.28 | 19.73x |
| threadtest-256 | 14 | 101.48 | 2005.16 | 19.76x |
| threadtest-64 | 1 | 77.58 | 275.33 | 3.55x |
| threadtest-64 | 2 | 151.68 | 536.77 | 3.54x |
| threadtest-64 | 4 | 196.29 | 967.59 | 4.93x |
| threadtest-64 | 8 | 143.17 | 1863.93 | 13.02x |
| threadtest-64 | 14 | 235.97 | 2025.46 | 8.58x |
| xmalloc | 2 | 23.77 | 35.98 | 1.51x |
| xmalloc | 4 | 22.08 | 53.08 | 2.40x |
| xmalloc | 8 | 33.09 | 80.67 | 2.44x |
| xmalloc | 14 | 37.29 | 124.69 | 3.34x |

### Linux, glibc 2.39 (Ubuntu 24.04 arm64)

Run in a Docker container on the same Mac (14 virtual CPUs), so treat these as indicative until there are
numbers from native Linux hardware. glibc gives each thread its own cache and arena, so the gap is much smaller
than against libmalloc: about 1.3x to 2.5x when threads keep to their own blocks. The largest gap is `xmalloc`
(4x to 5x), where every free comes from another thread. `cache_scratch` is level at 1 and 2 threads.

| bench | threads | system Mops/s | ts Mops/s | ts / system |
|---|---:|---:|---:|---:|
| cache_scratch | 1 | 3839.47 | 3839.06 | 1.00x |
| cache_scratch | 2 | 7457.23 | 7478.90 | 1.00x |
| cache_scratch | 4 | 4609.25 | 13357.30 | 2.90x |
| cache_scratch | 8 | 4536.80 | 26119.63 | 5.76x |
| larson | 1 | 59.71 | 76.94 | 1.29x |
| larson | 2 | 102.61 | 138.14 | 1.35x |
| larson | 4 | 140.36 | 186.04 | 1.33x |
| larson | 8 | 202.28 | 274.44 | 1.36x |
| threadtest-1024 | 1 | 75.29 | 147.34 | 1.96x |
| threadtest-1024 | 2 | 145.63 | 284.56 | 1.95x |
| threadtest-1024 | 4 | 267.90 | 521.65 | 1.95x |
| threadtest-1024 | 8 | 522.42 | 1000.32 | 1.91x |
| threadtest-16 | 1 | 118.91 | 185.56 | 1.56x |
| threadtest-16 | 2 | 237.39 | 352.17 | 1.48x |
| threadtest-16 | 4 | 427.21 | 634.11 | 1.48x |
| threadtest-16 | 8 | 822.97 | 1202.00 | 1.46x |
| threadtest-256 | 1 | 70.57 | 176.00 | 2.49x |
| threadtest-256 | 2 | 137.70 | 333.05 | 2.42x |
| threadtest-256 | 4 | 252.79 | 606.68 | 2.40x |
| threadtest-256 | 8 | 488.07 | 1156.71 | 2.37x |
| threadtest-64 | 1 | 117.61 | 182.54 | 1.55x |
| threadtest-64 | 2 | 235.01 | 352.96 | 1.50x |
| threadtest-64 | 4 | 426.19 | 621.08 | 1.46x |
| threadtest-64 | 8 | 821.74 | 1176.21 | 1.43x |
| xmalloc | 2 | 12.36 | 55.57 | 4.50x |
| xmalloc | 4 | 23.12 | 99.70 | 4.31x |
| xmalloc | 8 | 20.44 | 105.15 | 5.14x |

### Linux x86-64, glibc (GitHub Actions `ubuntu-latest`, gcc, 2026-11-10)

From the CI release job: `bench_override.sh --quick --threads="1 4" --reps=1` on a shared 4-vCPU runner,
so one short run per cell. The first native x86-64 numbers, and the pattern matches Docker: `xmalloc` is
where tsmalloc gains most, and `larson` is level.

| bench | threads | system Mops/s | ts Mops/s | ts / system |
|---|---:|---:|---:|---:|
| cache_scratch | 1 | 3533.05 | 3482.72 | 0.99x |
| cache_scratch | 4 | 2017.39 | 7180.21 | 3.56x |
| larson | 1 | 21.42 | 22.54 | 1.05x |
| larson | 4 | 34.67 | 29.99 | 0.87x |
| threadtest-1024 | 1 | 33.12 | 73.74 | 2.23x |
| threadtest-1024 | 4 | 71.60 | 116.11 | 1.62x |
| threadtest-16 | 1 | 47.75 | 88.15 | 1.85x |
| threadtest-16 | 4 | 77.89 | 100.06 | 1.28x |
| threadtest-256 | 1 | 50.14 | 85.33 | 1.70x |
| threadtest-256 | 4 | 105.89 | 137.45 | 1.30x |
| threadtest-64 | 1 | 49.98 | 89.22 | 1.79x |
| threadtest-64 | 4 | 105.01 | 146.02 | 1.39x |
| xmalloc | 2 | 3.82 | 22.52 | 5.90x |
| xmalloc | 4 | 5.69 | 37.23 | 6.54x |

## Against jemalloc and mimalloc (step 44)

`scripts/bench.sh --others` runs the same `*_plain` programs as `--override`, with jemalloc and mimalloc
injected too, if their shared libraries are already installed (Homebrew, Debian/Ubuntu multiarch paths, or
`/usr/local/lib`). It never installs anything. `TS_JEMALLOC=<path>` and `TS_MIMALLOC=<path>` point it at a
library somewhere else. The table gets one Mops/s column per allocator and a `ts / <name>` ratio for each.

Each run has to say which allocator served it, as with `--override`: a plain program looks for a symbol only
that allocator exports (`mi_version` for mimalloc, `mallctl` or `je_mallctl` for jemalloc), and the script
stops if the name doesn't match. Without that check a library that failed to load would quietly be measured
as the system allocator.

Neither library is installed on the development Mac or in the CI images, so there are no numbers here yet.
mimalloc is the allocator tsmalloc's design follows, so it is the comparison that would say most about what
the simplifications cost.
