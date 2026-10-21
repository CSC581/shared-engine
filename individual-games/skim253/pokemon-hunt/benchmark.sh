#!/usr/bin/env bash
# Times Pokemon Hunt's own game loop under the engine's Part 1C experiments,
# five repetitions each. Pick one experiment per run:
#
#   (default)            text vs binary, 1/2/4 clients, 10/100/500 platforms
#   --mixed              2/4 clients on one server as all-text, all-binary and
#                        mixed (even clients text, odd binary)
#   --snapshot-delivery  binary full snapshots vs binary property deltas,
#                        1/2/4 clients, 10/100/500 platforms
#
#   ./individual-games/skim253/pokemon-hunt/benchmark.sh [--mixed | --snapshot-delivery]
#   ITERATIONS=1000 REPS=1 ./individual-games/skim253/pokemon-hunt/benchmark.sh   # quick try
#
# Every run starts a fresh pokemon-hunt-server and one game window per
# client. Each client times ITERATIONS game-loop iterations once the whole
# party has joined and prints a PH_BENCHMARK line (see timeBenchmark() in
# PokemonHunt.cpp). Results land in build/performance-results/<UTC>-pokemon-hunt[-<experiment>]/:
#   runs.csv     one row per client per run
#   summary.csv  per condition and client format: mean and sample variance
#                over the repetitions of each run's mean update and whole-loop time
#
# Unlike the engine's --snapshot-delivery, the game sends at its own paced rate
# with its own player data, so it does not fix 60 requests/s or 0/512 B data.
set -euo pipefail

experiment=default
case "${1:-}" in
    "") ;;
    --mixed) experiment=mixed ;;
    --snapshot-delivery) experiment=snapshot-delivery ;;
    *) echo "usage: $0 [--mixed | --snapshot-delivery]" >&2; exit 1 ;;
esac

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="${BUILD:-$root/build-release}"
iterations="${ITERATIONS:-3000}"
reps="${REPS:-5}"
run_timeout=180
species=(pikachu charmander squirtle bulbasaur)

suffix=""
[[ "$experiment" != default ]] && suffix="-$experiment"
out="$root/build/performance-results/$(date -u +%Y%m%dT%H%M%SZ)-pokemon-hunt$suffix"
mkdir -p "$out/logs"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --target pokemon-hunt pokemon-hunt-server -j 4 > /dev/null

{
    echo "collected_utc=$(basename "$out" | cut -d- -f1)"
    echo "git_commit=$(git -C "$root" rev-parse HEAD)"
    echo "build_type=Release"
    echo "experiment=$experiment"
    echo "iterations=$iterations"
    echo "repetitions=$reps"
    echo "os=$(uname -srm)"
    echo "cpu_count=$(getconf _NPROCESSORS_ONLN)"
} > "$out/metadata.txt"

echo "condition,client_format,snapshots,clients,platforms,repetition,client,iterations,loop_seconds,update_seconds,party_seen,platforms_seen" \
    > "$out/runs.csv"

stop() {
    kill "$@" 2> /dev/null || true
    wait "$@" 2> /dev/null || true
}

case "$experiment" in
    default)
        client_counts=(1 2 4)
        conditions=(text binary) ;;
    mixed)
        client_counts=(2 4)
        conditions=(text binary mixed) ;;
    snapshot-delivery)
        client_counts=(1 2 4)
        conditions=(full delta) ;;
esac

# Sets client_format and snapshots for client $2 under condition $1.
client_setup() {
    snapshots=full
    case "$1" in
        text | binary) client_format=$1 ;;
        mixed) (($2 % 2 == 0)) && client_format=text || client_format=binary ;;
        full | delta) client_format=binary; snapshots=$1 ;;
    esac
}

for rep in $(seq 1 "$reps"); do
    for clients in "${client_counts[@]}"; do
        for platforms in 10 100 500; do
            # Every condition side by side, so drift on the machine hits all of them.
            for condition in "${conditions[@]}"; do
                tag="$condition-c$clients-p$platforms-r$rep"
                echo "run $tag"
                "$build/pokemon-hunt-server" --platforms "$platforms" > "$out/logs/$tag-server.log" 2>&1 &
                server=$!
                sleep 1

                pids=()
                for ((c = 0; c < clients; ++c)); do
                    client_setup "$condition" "$c"
                    "$build/pokemon-hunt" --pokemon "${species[$c]}" --format "$client_format" \
                        --snapshots "$snapshots" --benchmark "$iterations" --benchmark-party "$clients" \
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
                    client_setup "$condition" "$c"
                    field() { sed -E "s/.* $1=([^ ]+).*/\1/" <<< "$line"; }
                    echo "$condition,$client_format,$snapshots,$clients,$platforms,$rep,$c,$(field iterations)," \
                         "$(field seconds),$(field update_seconds),$(field party),$(field platforms)" \
                        | tr -d ' ' >> "$out/runs.csv"
                done
            done
        done
    done
done

# A run's time is the mean over its clients of one format; a condition's
# numbers are the mean and sample variance of that over the repetitions. Mixed
# runs therefore get one text row and one binary row per condition.
awk -F, '
NR == 1 { next }
{
    key = $1 FS $2 FS $3 FS $4 FS $5
    run = key FS $6
    loop[run] += $9; update[run] += $10; count[run]++
    if ($11 != $4 || $12 != $5) bad[key]++
}
END {
    for (run in count) {
        split(run, k, FS); cond = k[1] FS k[2] FS k[3] FS k[4] FS k[5]
        l = loop[run] / count[run]; u = update[run] / count[run]
        n[cond]++; ls[cond] += l; lq[cond] += l * l; us[cond] += u; uq[cond] += u * u
    }
    print "condition,client_format,snapshots,clients,platforms,runs,update_mean_seconds,update_sample_variance,loop_mean_seconds,loop_sample_variance,mismatched_rows"
    for (cond in n) {
        m = n[cond]
        um = us[cond] / m; lm = ls[cond] / m
        uv = m > 1 ? (uq[cond] - m * um * um) / (m - 1) : 0
        lv = m > 1 ? (lq[cond] - m * lm * lm) / (m - 1) : 0
        printf "%s,%d,%.6f,%.8f,%.6f,%.8f,%d\n", cond, m, um, uv, lm, lv, bad[cond] + 0
    }
}' "$out/runs.csv" | { read -r header; echo "$header"; sort -t, -k4,4n -k5,5n -k1,1 -k2,2; } > "$out/summary.csv"

echo "RESULT_DIR=$out"
column -s, -t < "$out/summary.csv"
