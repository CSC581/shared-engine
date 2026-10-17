#!/usr/bin/env bash
# Collect the equal-rate binary-full versus binary-delta experiment in Release.
# The primary matrix is canonical evidence. A shorter separate recovery matrix
# forces regular full resynchronizations without contaminating steady-state data.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
release_build=${1:-"${repo_root}/build-release"}
result_root=${2:-"${repo_root}/build/performance-results"}
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
result_dir="${result_root}/${timestamp}-snapshot-delivery"
recovery_dir="${result_dir}/recovery"
evidence_dir="${repo_root}/docs/performance-results/${timestamp}-snapshot-delivery"

mkdir -p "${result_dir}" "${recovery_dir}"
cmake -S "${repo_root}" -B "${release_build}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${release_build}" --target network-performance-benchmark -j 4

{
    echo "collected_utc=${timestamp}"
    echo "git_commit=$(git -C "${repo_root}" rev-parse HEAD)"
    echo "git_status_begin"
    git -C "${repo_root}" status --short
    echo "git_status_end"
    echo "build_type=Release"
    echo "experiment=binary-full_vs_binary-property-delta"
    echo "matrix=strategies(full,delta);clients(1,2,4);platforms(10,100,500);player_data_bytes(0,512);repetitions(5)"
    echo "offered_hz_per_client=60"
    echo "warmup_iterations_per_client=100"
    echo "measured_iterations_per_client=1000"
    echo "payload_scope=ZeroMQ frame contents; excludes ZeroMQ/TCP/IP framing"
    echo "baseline_memory_scope=encoded full-snapshot byte proxy per client; not allocator-resident bytes"
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

"${release_build}/network-performance-benchmark" --snapshot-delivery \
    --runs "${result_dir}/runs.csv" \
    --latency "${result_dir}/latency_samples.csv" \
    2>&1 | tee "${result_dir}/benchmark.log"

python3 "${repo_root}/benchmarks/analyze_snapshot_delivery.py" "${result_dir}" \
    2>&1 | tee "${result_dir}/analysis.log"

# Recovery is deliberately separate and shorter. Every delta client explicitly
# requests a complete snapshot three times in 120 updates. Stale-ACK and rejoin
# recovery remain deterministic integration tests rather than timing noise.
"${release_build}/network-performance-benchmark" --snapshot-delivery \
    --repetitions 1 --warmup 20 --iterations 120 --resync-every 30 \
    --runs "${recovery_dir}/runs.csv" \
    --latency "${recovery_dir}/latency_samples.csv" \
    2>&1 | tee "${recovery_dir}/benchmark.log"

python3 "${repo_root}/benchmarks/analyze_snapshot_delivery.py" "${recovery_dir}" \
    --repetitions 1 --warmup 20 --iterations 120 --resync-every 30 \
    2>&1 | tee "${recovery_dir}/analysis.log"

if [[ -r /proc/loadavg ]]; then
    echo "load_after=$(< /proc/loadavg)" >> "${result_dir}/metadata.txt"
fi

mkdir -p "${evidence_dir}"
cp "${result_dir}/summary.csv" "${result_dir}/metadata.txt" \
   "${result_dir}/snapshot_payload.svg" "${result_dir}/snapshot_rtt.svg" \
   "${result_dir}/snapshot_rate.svg" "${result_dir}/snapshot_payload_reduction.svg" \
   "${evidence_dir}/"
cp "${recovery_dir}/summary.csv" "${evidence_dir}/recovery-summary.csv"

echo "RESULT_DIR=${result_dir}"
echo "EVIDENCE_DIR=${evidence_dir}"
