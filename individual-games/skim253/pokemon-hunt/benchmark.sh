#!/usr/bin/env bash
# Times Pokemon Hunt's own game loop with text and binary client-server data,
# 1/2/4 clients and 10/100/500 moving platforms, five repetitions each.
#
#   ./individual-games/skim253/pokemon-hunt/benchmark.sh
#   ITERATIONS=1000 REPS=1 ./individual-games/skim253/pokemon-hunt/benchmark.sh   # quick try
#
# Every run starts a fresh pokemon-hunt-server and one game window per
# client. Each client times ITERATIONS game-loop iterations once the whole
# party has joined and prints a PH_BENCHMARK line (see timeBenchmark() in
# PokemonHunt.cpp). Results land in build/performance-results/<UTC>-pokemon-hunt/:
#   runs.csv     one row per client per run
#   summary.csv  per condition: mean and sample variance over the repetitions
#                of each run's mean update and whole-loop time
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="${BUILD:-$root/build-release}"
iterations="${ITERATIONS:-3000}"
reps="${REPS:-5}"
run_timeout=180
species=(pikachu charmander squirtle bulbasaur)

out="$root/build/performance-results/$(date -u +%Y%m%dT%H%M%SZ)-pokemon-hunt"
mkdir -p "$out/logs"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --target pokemon-hunt pokemon-hunt-server -j 4 > /dev/null

{
    echo "collected_utc=$(basename "$out" | cut -d- -f1)"
    echo "git_commit=$(git -C "$root" rev-parse HEAD)"
    echo "build_type=Release"
    echo "iterations=$iterations"
    echo "repetitions=$reps"
    echo "os=$(uname -srm)"
    echo "cpu_count=$(getconf _NPROCESSORS_ONLN)"
} > "$out/metadata.txt"

echo "format,clients,platforms,repetition,client,iterations,loop_seconds,update_seconds,party_seen,platforms_seen" \
    > "$out/runs.csv"

stop() {
    kill "$@" 2> /dev/null || true
    wait "$@" 2> /dev/null || true
}

for rep in $(seq 1 "$reps"); do
    for clients in 1 2 4; do
        for platforms in 10 100 500; do
            # Text and binary side by side, so drift on the machine hits both.
            for format in text binary; do
                tag="$format-c$clients-p$platforms-r$rep"
                echo "run $tag"
                "$build/pokemon-hunt-server" --platforms "$platforms" > "$out/logs/$tag-server.log" 2>&1 &
                server=$!
                sleep 1

                pids=()
                for ((c = 0; c < clients; ++c)); do
                    "$build/pokemon-hunt" --pokemon "${species[$c]}" --format "$format" \
                        --benchmark "$iterations" --benchmark-party "$clients" \
                        > "$out/logs/$tag-client$c.log" 2>&1 &
                    pids+=($!)
                done

                deadline=$((SECONDS + run_timeout))
                while ((SECONDS < deadline)); do
                    done_count=$( (grep -l PH_BENCHMARK "$out/logs/$tag"-client*.log 2> /dev/null || true) | wc -l)
                    ((done_count >= clients)) && break
                    sleep 1
                done
                stop "${pids[@]}"
                stop "$server"

                for ((c = 0; c < clients; ++c)); do
                    line=$(grep PH_BENCHMARK "$out/logs/$tag-client$c.log" || true)
                    if [[ -z "$line" ]]; then
                        echo "  client $c timed out (see logs/$tag-client$c.log)" >&2
                        continue
                    fi
                    field() { sed -E "s/.* $1=([^ ]+).*/\1/" <<< "$line"; }
                    echo "$format,$clients,$platforms,$rep,$c,$(field iterations),$(field seconds)," \
                         "$(field update_seconds),$(field party),$(field platforms)" | tr -d ' ' >> "$out/runs.csv"
                done
            done
        done
    done
done

# A run's time is the mean over its clients; a condition's numbers are the
# mean and sample variance of that over the repetitions.
awk -F, '
NR == 1 { next }
{
    run = $1 FS $2 FS $3 FS $4
    loop[run] += $7; update[run] += $8; count[run]++
    if ($9 != $2 || $10 != $3) bad[$1 FS $2 FS $3]++
}
END {
    for (run in count) {
        split(run, k, FS); cond = k[1] FS k[2] FS k[3]
        l = loop[run] / count[run]; u = update[run] / count[run]
        n[cond]++; ls[cond] += l; lq[cond] += l * l; us[cond] += u; uq[cond] += u * u
    }
    print "format,clients,platforms,runs,update_mean_seconds,update_sample_variance,loop_mean_seconds,loop_sample_variance,mismatched_rows"
    for (cond in n) {
        m = n[cond]
        um = us[cond] / m; lm = ls[cond] / m
        uv = m > 1 ? (uq[cond] - m * um * um) / (m - 1) : 0
        lv = m > 1 ? (lq[cond] - m * lm * lm) / (m - 1) : 0
        printf "%s,%d,%.6f,%.8f,%.6f,%.8f,%d\n", cond, m, um, uv, lm, lv, bad[cond] + 0
    }
}' "$out/runs.csv" | { read -r header; echo "$header"; sort -t, -k2,2n -k3,3n -k1,1r; } > "$out/summary.csv"

echo "RESULT_DIR=$out"
column -s, -t < "$out/summary.csv"
