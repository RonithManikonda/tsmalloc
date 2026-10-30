# tsmalloc

A thread-safe `malloc` replacement written in C11. The goal is to beat the system allocator on
multithreaded (`-pthread`) programs: macOS libmalloc on my machine, glibc's ptmalloc on Linux.

## Design sketch

The design borrows the core ideas of [mimalloc](https://github.com/microsoft/mimalloc), kept small
enough to explain end to end:

- **Segments.** Memory comes from the OS in 4 MiB chunks aligned to 4 MiB, so the metadata for any
  pointer is found with a single mask: `p & ~(4 MiB - 1)`.
- **Pages.** Each segment is cut into 64 KiB pages. A page holds blocks of a single size class.
- **Thread-local heaps.** Every thread allocates from pages it owns, so the common path takes no
  locks and touches no shared cache lines.
- **Three free lists per page.** `free` is what the owner allocates from, `local_free` collects the
  owner's own frees, and an atomic `thread_free` list lets other threads hand blocks back with one
  CAS. The owner grabs the whole remote list with a single atomic exchange.
- **Abandonment.** When a thread exits, its segments are marked abandoned and adopted later by
  threads that need memory.
- Large allocations go straight to `mmap`.

## Status

Early days. Small sizes (up to 16 KiB) come from pages in 4 MiB segments. Every thread gets its own
heap, with a page queue per size class, on its first malloc. Allocating and freeing your own blocks
takes no lock. A block freed by another thread is pushed with one CAS onto its page's `thread_free`
list, and the owner splices that list back in when the page runs dry. Heaps of exited threads aren't
reclaimed yet. Larger sizes get their own `mmap`'d region. There are no per-block headers: `ts_free` masks the pointer to find its
segment or mapping and reads the block size from there.

Benchmarks and the first numbers against macOS libmalloc are in
[docs/benchmarks.md](docs/benchmarks.md). Run them with `scripts/bench.sh` (`--quick` for a short
sanity run).

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer builds: `-DTS_SANITIZER=address|thread|undefined` (use a separate build directory each,
e.g. `build-asan/`).
