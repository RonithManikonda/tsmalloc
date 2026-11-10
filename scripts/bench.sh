#!/usr/bin/env bash
#
# Runs every benchmark against the system allocator and tsmalloc at a range
# of thread counts and prints a markdown table (median Mops/s of --reps runs).
#
#   scripts/bench.sh                    # 1 2 4 8 ncpu threads, 3 reps
#   scripts/bench.sh --threads="1 4" --reps=1 --quick
#
# Builds into build-rel/ (Release) first. --quick divides the work per thread
# by 10, for a sanity check rather than numbers worth publishing.
#
# --override compares the *_plain programs instead, which only call libc's
# malloc and free: once as they are and once with tsmalloc injected. Each run
# has to report the allocator it was meant to use, or the script stops.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-rel"

ncpu="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
threads="1 2 4 8 $ncpu"
reps=3
scale=1
override=0

for arg in "$@"; do
    case "$arg" in
        --threads=*) threads="${arg#*=}" ;;
        --reps=*)    reps="${arg#*=}" ;;
        --quick)     scale=10 ;;
        --override)  override=1 ;;
        *) echo "usage: $0 [--threads=\"1 2 4\"] [--reps=N] [--quick] [--override]" >&2; exit 2 ;;
    esac
done

# Drop duplicates (ncpu may already be in the list) and sort.
threads="$(printf '%s\n' $threads | sort -n -u | tr '\n' ' ')"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DTS_SANITIZER=none >/dev/null
cmake --build "$build" -j "$ncpu" >/dev/null

# Work per thread, a quarter of each program's default or less. A full sweep
# takes ~10 s on 14 cores (it was ~18 minutes with the global-lock heap,
# nearly all of it waiting on the mutex); --quick is the fast check.
benches=(
    "threadtest    $((5000 / scale))"
    "larson        $((500000 / scale))"
    "xmalloc       $((500000 / scale))"
    "cache_scratch $((20000 / scale))"
)

if [ "$(uname)" = Darwin ]; then
    preload_var=DYLD_INSERT_LIBRARIES
    preload_lib="$build/libtsmalloc.dylib"
else
    preload_var=LD_PRELOAD
    preload_lib="$build/libtsmalloc.so"
fi

# One run, printing the program's result lines.
run_bench() {
    local bench="$1" alloc="$2" t="$3" iters="$4" out
    if [ "$override" -eq 0 ]; then
        "$build/bench/$bench" --alloc="$alloc" --threads="$t" --iters="$iters"
        return
    fi
    if [ "$alloc" = ts ]; then
        out="$(env "$preload_var=$preload_lib" "$build/bench/${bench}_plain" --threads="$t" --iters="$iters")"
    else
        out="$("$build/bench/${bench}_plain" --threads="$t" --iters="$iters")"
    fi
    if ! grep -q "alloc=$alloc " <<<"$out"; then
        echo "bench.sh: $bench did not run on $alloc:" >&2
        echo "$out" >&2
        exit 1
    fi
    echo "$out"
}

# bench -> "name threads alloc mops" lines, one per printed result.
results="$(mktemp)"
trap 'rm -f "$results"' EXIT

for entry in "${benches[@]}"; do
    read -r bench iters <<<"$entry"
    for t in $threads; do
        for alloc in system ts; do
            for ((r = 0; r < reps; r++)); do
                run_bench "$bench" "$alloc" "$t" "$iters" |
                    sed -E 's/.*bench=([^ ]+) alloc=([^ ]+) threads=([0-9]+).* mops=([0-9.]+).*/\1 \3 \2 \4/' \
                    >>"$results"
            done
            echo "  $bench threads=$t alloc=$alloc done" >&2
        done
    done
done

# xmalloc rounds odd thread counts up, so key rows by the count it reports.
echo "| bench | threads | system Mops/s | ts Mops/s | ts / system |"
echo "|---|---:|---:|---:|---:|"
awk '
    { key = $1 " " $2; v[key, $3, ++n[key, $3]] = $4; seen[key] = 1 }
    function median(key, alloc,    c, i, j, t, a) {
        c = n[key, alloc]
        for (i = 1; i <= c; i++) a[i] = v[key, alloc, i]
        for (i = 2; i <= c; i++)
            for (j = i; j > 1 && a[j - 1] > a[j]; j--) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t }
        return c % 2 ? a[(c + 1) / 2] : (a[c / 2] + a[c / 2 + 1]) / 2
    }
    END {
        for (key in seen) {
            s = median(key, "system"); t = median(key, "ts")
            split(key, k, " ")
            # The sort key goes in front, then cut drops it.
            printf "%s\t%d\t| %s | %d | %.2f | %.2f | %.2fx |\n", k[1], k[2], k[1], k[2], s, t, (s > 0 ? t / s : 0)
        }
    }' "$results" | sort -t"$(printf '\t')" -k1,1 -k2,2n | cut -f3-
