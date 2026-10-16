#!/usr/bin/env bash
# Runs optional engine diagnostic benchmarks with their full matrices in a
# Release build. These complement, but do not replace, the required network
# format comparison and its canonical evidence.
#
#   ./benchmarks/collect_engine_performance.sh [release_build] [result_root]
#   BENCHMARKS="collision codec" ./benchmarks/collect_engine_performance.sh
#
# Results land in build/performance-results/<UTC>-engine/ with one
# <name>-runs.csv and <name>-summary.csv per benchmark, plus metadata.txt.
# multiplayer-mode needs loopback sockets and takes a few minutes.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
release_build=${1:-"${repo_root}/build-release"}
result_root=${2:-"${repo_root}/build/performance-results"}
benchmarks=${BENCHMARKS:-"collision component-update frame-workers codec multiplayer-mode snapshot-strategy"}
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
result_dir="${result_root}/${timestamp}-engine"

mkdir -p "${result_dir}"

expected_rows() {
    case "$1" in
        collision) echo "75 15" ;;
        component-update) echo "75 15" ;;
        frame-workers) echo "100 20" ;;
        codec) echo "120 24" ;;
        multiplayer-mode) echo "45 9" ;;
        snapshot-strategy) echo "405 81" ;;
        *) echo "unknown benchmark: $1" >&2; return 1 ;;
    esac
}

validate_results() {
    local name=$1
    local runs_file="${result_dir}/${name}-runs.csv"
    local summary_file="${result_dir}/${name}-summary.csv"
    local expected_run_rows expected_summary_rows
    read -r expected_run_rows expected_summary_rows < <(expected_rows "${name}")

    awk -F, -v expected="${expected_run_rows}" '
        NR == 1 { if (NF < 3) exit 1; next }
        { rows++ }
        END { if (rows != expected) exit 1 }
    ' "${runs_file}" || { echo "invalid run matrix for ${name}" >&2; return 1; }

    awk -F, -v expected="${expected_summary_rows}" '
        NR == 1 {
            for (column = 1; column <= NF; ++column) {
                if ($column == "runs") runs_column = column
            }
            if (!runs_column) exit 1
            next
        }
        {
            rows++
            if ($runs_column != 5) exit 1
            for (column = 1; column <= NF; ++column) {
                lower = tolower($column)
                if (lower == "nan" || lower == "inf" || lower == "-inf") exit 1
            }
        }
        END { if (rows != expected) exit 1 }
    ' "${summary_file}" || { echo "invalid summary matrix for ${name}" >&2; return 1; }
}

targets=()
for name in ${benchmarks}; do
    expected_rows "${name}" >/dev/null
    targets+=("${name}-benchmark")
done
cmake -S "${repo_root}" -B "${release_build}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${release_build}" --target "${targets[@]}" -j 4

{
    echo "collected_utc=${timestamp}"
    echo "git_commit=$(git -C "${repo_root}" rev-parse HEAD)"
    echo "git_status_begin"
    git -C "${repo_root}" status --short
    echo "git_status_end"
    echo "build_type=Release"
    echo "benchmarks=${benchmarks}"
    echo "repetitions=5"
    echo "os=$(uname -srm)"
    echo "cpu_count=$(getconf _NPROCESSORS_ONLN)"
    if command -v sysctl >/dev/null 2>&1; then
        sysctl -n machdep.cpu.brand_string 2>/dev/null | sed 's/^/cpu_model=/' || true
    fi
    if command -v lscpu >/dev/null 2>&1; then
        lscpu | sed -n 's/^Model name:[[:space:]]*/cpu_model=/p'
    fi
    cmake --version | sed -n '1s/^/cmake=/p'
    c++ --version | sed -n '1s/^/compiler=/p'
} > "${result_dir}/metadata.txt"

for name in ${benchmarks}; do
    echo "== ${name}"
    "${release_build}/${name}-benchmark" \
        --runs "${result_dir}/${name}-runs.csv" \
        --summary "${result_dir}/${name}-summary.csv" \
        2>&1 | tee "${result_dir}/${name}.log"
    validate_results "${name}"
done

echo "RESULT_DIR=${result_dir}"
