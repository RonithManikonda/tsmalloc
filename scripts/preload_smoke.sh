#!/usr/bin/env bash
#
# Runs real programs with tsmalloc preloaded and checks they do the same
# work as without it. Linux only; registered with ctest as preload_smoke.
#
#   scripts/preload_smoke.sh build/libtsmalloc.so
#
# A tool that isn't installed is skipped, not failed, so this also runs on
# minimal images. What runs is listed at the end.
set -euo pipefail

if [ $# -ne 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 path/to/libtsmalloc.so" >&2
    exit 2
fi

lib="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

ran=()
skipped=()

with_ts() {
    LD_PRELOAD="$lib" "$@"
}

have() {
    if command -v "$1" >/dev/null 2>&1; then
        ran+=("$1")
        return 0
    fi
    skipped+=("$1")
    return 1
}

# Every check below would pass just as well if the loader had ignored the
# library, so first make sure it doesn't.
if ! with_ts cat /proc/self/maps | grep -qF "$lib"; then
    echo "preload_smoke: $lib was not loaded" >&2
    exit 1
fi

if have ls; then
    ls -la /usr/lib /usr/bin >ls.plain
    with_ts ls -la /usr/lib /usr/bin >ls.ts
    cmp ls.plain ls.ts
fi

if have git; then
    # Plenty of small allocations, plus child processes that inherit the
    # preload. Hashes are compared, so the dates have to be fixed.
    export GIT_AUTHOR_DATE="2000-01-01T00:00:00Z" GIT_COMMITTER_DATE="2000-01-01T00:00:00Z"
    identity=(-c user.name=smoke -c user.email=smoke@example.invalid -c gc.auto=0)
    for side in plain ts; do
        run=()
        [ "$side" = ts ] && run=(with_ts)
        mkdir "repo.$side"
        (
            cd "repo.$side"
            "${run[@]}" git init -q -b main .
            for i in $(seq 1 200); do
                echo "file $i" >"f$i.txt"
            done
            "${run[@]}" git add .
            "${run[@]}" git "${identity[@]}" commit -q -m first
            seq 1 2000 >f1.txt
            "${run[@]}" git "${identity[@]}" commit -q -am second
            "${run[@]}" git gc -q
            "${run[@]}" git fsck --strict
            "${run[@]}" git log --format=%H >../git.$side
        )
    done
    cmp git.plain git.ts
fi

cat >hello.c <<'CODE'
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void *work(void *arg) {
    return arg;
}

int main(void) {
    pthread_t t;
    void *out = NULL;
    char *p = malloc(64);
    pthread_create(&t, NULL, work, p);
    pthread_join(t, &out);
    puts(out == p ? "ok" : "wrong");
    free(p);
    return 0;
}
CODE

cat >hello.cpp <<'CODE'
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

int main() {
    std::map<std::string, std::vector<int>> m;
    for (int i = 0; i < 1000; i++)
        m[std::to_string(i % 37)].push_back(i);
    std::size_t total = 0;
    for (const auto &kv : m)
        total += kv.second.size();
    std::cout << (total == 1000 ? "ok" : "wrong") << "\n";
}
CODE

# The compiler runs preloaded; the program it built runs normally and then
# preloaded, which covers libstdc++'s operator new as well.
for compiler in gcc g++; do
    have "$compiler" || continue
    src=hello.c
    [ "$compiler" = g++ ] && src=hello.cpp
    with_ts "$compiler" -O2 -pthread -o "out.$compiler" "$src"
    [ "$("./out.$compiler")" = ok ]
    [ "$(with_ts "./out.$compiler")" = ok ]
done

if have python3; then
    with_ts python3 "$root/test/override_smoke.py"
fi

[ "${#ran[@]}" -gt 0 ] || { echo "preload_smoke: nothing to run" >&2; exit 1; }
echo "preload_smoke: ok (${ran[*]})${skipped[*]:+, skipped: ${skipped[*]}}"
