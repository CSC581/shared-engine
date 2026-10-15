#!/usr/bin/env bash
# Runs the five engine benchmarks with their full matrices in a Release build.
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
benchmarks=${BENCHMARKS:-"collision component-update frame-workers codec multiplayer-mode"}
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
result_dir="${result_root}/${timestamp}-engine"

mkdir -p "${result_dir}"

targets=()
for name in ${benchmarks}; do
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
done

echo "RESULT_DIR=${result_dir}"
