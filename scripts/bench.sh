#!/usr/bin/env bash
#
# Runs every benchmark against the system allocator and tsmalloc at a range
# of thread counts and prints a markdown table (median Mops/s of --reps runs).
#
#   scripts/bench.sh                    # 1 2 4 8 ncpu threads, 3 reps
#   scripts/bench.sh --threads="1 4" --reps=1 --quick
#   scripts/bench.sh --others           # also jemalloc / mimalloc, if installed
#
# Builds into build-rel/ (Release) first. --quick divides the work per thread
# by 10, for a sanity check rather than numbers worth publishing.
#
# --override compares the *_plain programs instead, which only call libc's
# malloc and free: once as they are and once with tsmalloc injected. Each run
# has to report the allocator it was meant to use, or the script stops.
#
# --others (implies --override) adds jemalloc and mimalloc columns when their
# shared libraries are already on the machine. Nothing is ever installed; set
# TS_JEMALLOC / TS_MIMALLOC to a library path to use one from elsewhere.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-rel"

ncpu="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
threads="1 2 4 8 $ncpu"
reps=3
scale=1
override=0
others=0

for arg in "$@"; do
    case "$arg" in
        --threads=*) threads="${arg#*=}" ;;
        --reps=*)    reps="${arg#*=}" ;;
        --quick)     scale=10 ;;
        --override)  override=1 ;;
        --others)    override=1; others=1 ;;
        *) echo "usage: $0 [--threads=\"1 2 4\"] [--reps=N] [--quick] [--override] [--others]" >&2; exit 2 ;;
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

# The library to inject for an allocator; empty for the system one.
lib_path() {
    case "$1" in
        ts)       echo "$preload_lib" ;;
        jemalloc) echo "$jemalloc_lib" ;;
        mimalloc) echo "$mimalloc_lib" ;;
    esac
}

# First existing file among the arguments, or nothing.
first_file() {
    local f
    for f in "$@"; do
        if [ -f "$f" ]; then
            echo "$f"
            return
        fi
    done
}

allocs="system ts"
jemalloc_lib=""
mimalloc_lib=""
if [ "$others" -eq 1 ]; then
    # Homebrew (arm64 and Intel), Debian/Ubuntu multiarch, /usr/local builds.
    jemalloc_lib="${TS_JEMALLOC:-$(first_file \
        /opt/homebrew/opt/jemalloc/lib/libjemalloc.dylib \
        /usr/local/opt/jemalloc/lib/libjemalloc.dylib \
        /usr/lib/*-linux-gnu/libjemalloc.so.2 \
        /usr/local/lib/libjemalloc.so.2 /usr/local/lib/libjemalloc.dylib)}"
    mimalloc_lib="${TS_MIMALLOC:-$(first_file \
        /opt/homebrew/opt/mimalloc/lib/libmimalloc.dylib \
        /usr/local/opt/mimalloc/lib/libmimalloc.dylib \
        /usr/lib/*-linux-gnu/libmimalloc.so.2 \
        /usr/local/lib/libmimalloc.so.2 /usr/local/lib/libmimalloc.dylib)}"
    for name in jemalloc mimalloc; do
        lib="$(lib_path "$name")"
        if [ -n "$lib" ] && [ -f "$lib" ]; then
            echo "  $name: $lib" >&2
            allocs="$allocs $name"
        else
            echo "  $name: not found, skipped" >&2
        fi
    done
fi

# One run, printing the program's result lines.
run_bench() {
    local bench="$1" alloc="$2" t="$3" iters="$4" out
    if [ "$override" -eq 0 ]; then
        "$build/bench/$bench" --alloc="$alloc" --threads="$t" --iters="$iters"
        return
    fi
    if [ "$alloc" != system ]; then
        out="$(env "$preload_var=$(lib_path "$alloc")" "$build/bench/${bench}_plain" --threads="$t" --iters="$iters")"
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
        for alloc in $allocs; do
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
# One Mops/s column per allocator, then tsmalloc relative to each of the others.
header="| bench | threads |"
rule="|---|---:|"
for a in $allocs; do header="$header $a Mops/s |"; rule="$rule---:|"; done
for a in $allocs; do
    [ "$a" = ts ] && continue
    header="$header ts / $a |"; rule="$rule---:|"
done
echo "$header"
echo "$rule"
awk -v allocs="$allocs" '
    { key = $1 " " $2; v[key, $3, ++n[key, $3]] = $4; seen[key] = 1 }
    function median(key, alloc,    c, i, j, t, a) {
        c = n[key, alloc]
        for (i = 1; i <= c; i++) a[i] = v[key, alloc, i]
        for (i = 2; i <= c; i++)
            for (j = i; j > 1 && a[j - 1] > a[j]; j--) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t }
        return c % 2 ? a[(c + 1) / 2] : (a[c / 2] + a[c / 2 + 1]) / 2
    }
    END {
        na = split(allocs, al, " ")
        for (key in seen) {
            split(key, k, " ")
            for (i = 1; i <= na; i++) m[al[i]] = median(key, al[i])
            # The sort key goes in front, then cut drops it.
            line = sprintf("%s\t%d\t| %s | %d |", k[1], k[2], k[1], k[2])
            for (i = 1; i <= na; i++) line = line sprintf(" %.2f |", m[al[i]])
            for (i = 1; i <= na; i++) {
                if (al[i] == "ts") continue
                line = line sprintf(" %.2fx |", (m[al[i]] > 0 ? m["ts"] / m[al[i]] : 0))
            }
            print line
        }
    }' "$results" | sort -t"$(printf '\t')" -k1,1 -k2,2n | cut -f3-
