#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
release_build=${1:-"${repo_root}/build-release"}
result_root=${2:-"${repo_root}/build/performance-results"}
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
result_dir="${result_root}/${timestamp}"
evidence_dir="${repo_root}/docs/performance-results/${timestamp}"

mkdir -p "${result_dir}"

cmake -S "${repo_root}" -B "${release_build}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${release_build}" --target network-performance-benchmark -j 4

{
    echo "collected_utc=${timestamp}"
    echo "git_commit=$(git -C "${repo_root}" rev-parse HEAD)"
    echo "git_status_begin"
    git -C "${repo_root}" status --short
    echo "git_status_end"
    echo "build_type=Release"
    echo "benchmark=${release_build}/network-performance-benchmark"
    echo "matrix=formats(text,binary);clients(1,2,4);platforms(10,100,500);repetitions(5)"
    echo "warmup_iterations_per_client=100"
    echo "measured_iterations_per_client=1000"
    echo "os=$(uname -srmo)"
    echo "cpu_count=$(getconf _NPROCESSORS_ONLN)"
    if command -v lscpu >/dev/null 2>&1; then
        lscpu | sed -n -e 's/^Model name:[[:space:]]*/cpu_model=/p' \
                        -e 's/^Architecture:[[:space:]]*/architecture=/p'
    fi
    cmake --version | sed -n '1s/^/cmake=/p'
    c++ --version | sed -n '1s/^/compiler=/p'
    if [[ -r /proc/loadavg ]]; then
        echo "load_before=$(< /proc/loadavg)"
    fi
} > "${result_dir}/metadata.txt"

"${release_build}/network-performance-benchmark" \
    --runs "${result_dir}/runs.csv" \
    --latency "${result_dir}/latency_samples.csv" \
    2>&1 | tee "${result_dir}/benchmark.log"

if [[ -r /proc/loadavg ]]; then
    echo "load_after=$(< /proc/loadavg)" >> "${result_dir}/metadata.txt"
fi

python3 "${repo_root}/benchmarks/analyze_network_performance.py" "${result_dir}" \
    2>&1 | tee "${result_dir}/analysis.log"

# Raw runs and 210,000 latency samples remain in the ignored build tree. The
# compact, reviewable evidence is copied beside the networking documentation.
mkdir -p "${evidence_dir}"
cp "${result_dir}/summary.csv" "${result_dir}/metadata.txt" \
   "${result_dir}/elapsed_time.svg" "${result_dir}/throughput.svg" \
   "${result_dir}/payload_size.svg" "${result_dir}/binary_speedup.svg" \
   "${evidence_dir}/"

echo "RESULT_DIR=${result_dir}"
echo "EVIDENCE_DIR=${evidence_dir}"
