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

Early scaffolding. The public API (`include/tsmalloc.h`) exists and currently forwards to the
system allocator so tests can be written first. The real allocator replaces it piece by piece.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer builds: `-DTS_SANITIZER=address|thread|undefined` (use a separate build directory each,
e.g. `build-asan/`).
