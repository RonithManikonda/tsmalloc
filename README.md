# tsmalloc

[![ci](https://github.com/RonithManikonda/tsmalloc/actions/workflows/ci.yml/badge.svg)](https://github.com/RonithManikonda/tsmalloc/actions/workflows/ci.yml)

A thread-safe `malloc` replacement written in C11. The goal is to beat the system allocator on
multithreaded (`-pthread`) programs: macOS libmalloc on my machine, glibc's ptmalloc on Linux. It can be
called directly through its own `ts_` API, or dropped into an unmodified program in place of `malloc`.

## Headline numbers

Unmodified benchmark programs that call plain `malloc` and `free`, compiled with `-pthread`, run once
with the system allocator and once with tsmalloc injected. The table shows tsmalloc's throughput divided
by the system allocator's (above 1x means tsmalloc is faster), as a range over 1 to 14 threads:

| bench | macOS, M4 Pro | Linux arm64 (Docker) | Linux x86-64 (CI) |
|---|---:|---:|---:|
| `threadtest`: threads allocate and free their own blocks | 3.3x – 89x | 1.4x – 2.5x | 1.3x – 2.2x |
| `larson`: blocks handed between threads, server style | 3.1x – 12x | 1.3x – 1.4x | 0.87x – 1.05x |
| `xmalloc`: one thread allocates, another frees | 1.5x – 3.3x | 4.3x – 5.1x | 5.9x – 6.5x |
| `cache_scratch`: false sharing between threads' blocks | 1.05x – 4.4x | 1.0x – 5.8x | 0.99x – 3.6x |

On macOS tsmalloc is ahead in every cell, by an order of magnitude once several threads allocate at
the same time. On Linux it is ahead nearly everywhere, and furthest ahead when blocks cross threads. The
one place it only draws level is `larson` against x86-64 glibc, where glibc's per-thread cache reuses
another thread's block on the spot and tsmalloc sends it back to its owner. Method, machines, full tables
and the reasons behind each row are in [docs/benchmarks.md](docs/benchmarks.md).

## Using it

Build a Release copy first:

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
```

**Replace malloc in an existing program, Linux.** `libtsmalloc.so` defines `malloc`, `free`, `calloc`,
`realloc`, `aligned_alloc`, `posix_memalign`, `memalign`, `valloc`, `pvalloc`, `malloc_usable_size` and
`reallocarray`:

```sh
LD_PRELOAD=$PWD/build-rel/libtsmalloc.so ./your_program
```

Linking against `libtsmalloc.so`, or the static `libtsmalloc_override.a`, does the same without the
environment variable.

**Replace malloc in an existing program, macOS.** `libtsmalloc.dylib` interposes the malloc family and
registers itself as the default malloc zone, so system libraries that allocate through zones use it too:

```sh
DYLD_INSERT_LIBRARIES=$PWD/build-rel/libtsmalloc.dylib ./your_program
```

System Integrity Protection strips `DYLD_*` variables from Apple's own binaries (`/bin`, `/usr/bin`),
so this works on your programs and on things like Homebrew's tools, not on `/bin/ls`.

**Call it directly.** Link `libtsmalloc.a` (the CMake target `tsmalloc`) and include `tsmalloc.h`:

```c
#include <tsmalloc.h>

char *p = ts_malloc(100);
size_t n = ts_usable_size(p);   /* 112: the size class it came from */
ts_free(p);
```

The API is `ts_malloc`, `ts_free`, `ts_calloc`, `ts_realloc`, `ts_aligned_alloc`,
`ts_posix_memalign` and `ts_usable_size`, with the same contracts as their standard counterparts.

**Run the comparison yourself.** `scripts/bench_override.sh` runs the table above on your machine
(`--quick` for a short run), and `scripts/bench.sh --others` adds jemalloc and mimalloc columns if
either is already installed.

## How it works

The design borrows the core ideas of [mimalloc](https://github.com/microsoft/mimalloc), kept small
enough to explain end to end:

- **Segments.** Memory comes from the OS in 4 MiB chunks aligned to 4 MiB, so the metadata for any
  pointer is found with a single mask: `p & ~(4 MiB - 1)`. There are no per-block headers.
- **Pages.** Each segment is cut into 64 KiB pages. A page holds blocks of one size class (16 B to
  16 KiB). Medium sizes, up to 512 KiB, use spans of several pages in a row.
- **Thread-local heaps.** Every thread allocates from pages it owns, so the common path takes no
  locks and no atomics: a small `malloc` is a few loads and stores.
- **Three free lists per page.** `free` is what the owner allocates from, `local_free` collects the
  owner's own frees, and an atomic `thread_free` list lets other threads hand blocks back with one
  CAS. The owner takes the whole remote list with a single atomic exchange.
- **Abandonment.** When a thread exits, its heap is abandoned: free memory goes back, and segments
  that still hold live blocks wait until another thread adopts them.
- **Huge blocks** (over 512 KiB) get their own `mmap`, with a small cache of recently freed mappings.
  Pages that stay empty for a while are given back to the OS (`TSMALLOC_PURGE_DELAY` tunes how long).

[docs/design.md](docs/design.md) walks through all of it, including the memory-ordering argument
for why the remote-free path is safe, and how this compares with ptmalloc's arenas and libmalloc's
magazines.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer builds: `-DTS_SANITIZER=address|thread|undefined`, each in its own build directory (e.g.
`build-asan/`). `ctest -L stress` runs only the multithreaded stress tests; `TS_TEST_SCALE=<n>` makes
them run longer. The seeded fuzz test prints its seed, and `TS_FUZZ_SEED=<seed>` replays it.

Build options:

- `-DTS_DEBUG=ON` turns on checks in every free, realloc and `ts_usable_size`: double frees, pointers
  tsmalloc never handed out (or into the middle of a block), and writes past the end of a block (each
  block keeps an 8-byte canary after its usable size) abort with a message. Freed blocks are filled
  with `0xDF`. It applies to the override libraries too, so a whole program can run with it.
- `-DTS_STATS=ON` counts allocations, frees, remote frees, pages, segments and mmaps;
  `ts_stats_print(stderr)` prints them. Off by default, and free when off.

CI (`.github/workflows/ci.yml`) builds and tests on Ubuntu with gcc and clang and on macOS, in Debug,
ASan, UBSan, TSan and Release, and the Release jobs also run real programs (`ls`, `git`, `python3`, a
`gcc` compile) under `LD_PRELOAD` plus a quick benchmark sweep.

## Status

Feature-complete for what it set out to do: every malloc-family call, thread-local heaps with a
lock-free remote-free path, medium and huge sizes, purging, fork safety, drop-in overrides on Linux and
macOS, a debug mode, and stress, fuzz and sanitizer coverage in CI.

## Limitations

- **Memory use isn't measured.** The benchmarks measure throughput only. There's no coalescing, and
  each thread keeps its own partly used pages, so a program with many threads and small, scattered
  working sets can use more memory than ptmalloc.
- **Platforms.** 64-bit Linux and macOS only (x86-64 and arm64); the ownership registry assumes a
  48-bit address space. No Windows.
- **macOS tools.** The malloc zone doesn't enumerate its blocks, so `leaks` and `heap` see it as empty.
- **Foreign pointers on Linux.** With `LD_PRELOAD` tsmalloc is in place before the first allocation,
  so this shouldn't happen, but a pointer it didn't allocate is leaked by `free` and fails `realloc`
  rather than being passed back to glibc. (macOS forwards such pointers to the original zone.)
- **`realloc(p, 0)`.** Through the override on Linux it frees and returns `NULL`, like glibc. Through
  `ts_realloc` and on macOS it returns a minimal block, like libmalloc.
- **Benchmark coverage.** The Linux numbers come from Docker on a Mac and from short runs on shared CI
  machines; a dedicated Linux box is the missing measurement. jemalloc and mimalloc weren't installed
  anywhere it was measured, so there's no comparison against them yet.
