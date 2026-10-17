#!/usr/bin/env python3
"""Validate and summarize the equal-rate binary full-vs-delta experiment."""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

from analyze_network_performance import (
    CLIENTS,
    PLATFORMS,
    grouped_bar_svg,
    integer,
    load_csv,
    mean_variance,
    number,
    percentile,
    require,
)

STRATEGIES = ("full", "delta")
DATA_BYTES = (0, 512)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_dir", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--offered-hz", type=int, default=60)
    parser.add_argument("--resync-every", type=int, default=0)
    return parser.parse_args()


def validate_runs(rows: list[dict[str, str]], args: argparse.Namespace) -> dict[int, dict[str, str]]:
    expected_cells = {
        (strategy, clients, platforms, data_bytes, repetition)
        for strategy in STRATEGIES
        for clients in CLIENTS
        for platforms in PLATFORMS
        for data_bytes in DATA_BYTES
        for repetition in range(1, args.repetitions + 1)
    }
    require(len(rows) == len(expected_cells),
            f"runs.csv has {len(rows)} rows; expected {len(expected_cells)}")
    seen: set[tuple[str, int, int, int, int]] = set()
    by_id: dict[int, dict[str, str]] = {}
    for row in rows:
        run_id = integer(row, "run_id")
        cell = (row.get("snapshot_strategy", ""), integer(row, "clients"),
                integer(row, "platforms"), integer(row, "player_data_bytes"),
                integer(row, "repetition"))
        require(cell in expected_cells and cell not in seen,
                f"unexpected or duplicate matrix cell {cell}")
        require(row.get("format") == "binary", f"run {run_id} is not binary")
        require(integer(row, "warmup_iterations") == args.warmup,
                f"run {run_id} has the wrong warm-up count")
        require(integer(row, "measured_iterations") == args.iterations,
                f"run {run_id} has the wrong measured count")
        require(integer(row, "offered_hz") == args.offered_hz,
                f"run {run_id} has the wrong offered rate")
        clients = cell[1]
        expected = clients * args.iterations
        for field in ("completed_round_trips", "rtt_sample_count", "messages_sent",
                      "messages_received", "frames_sent", "frames_received",
                      "object_count_checks", "server_position_updates"):
            require(integer(row, field) == expected,
                    f"run {run_id} field {field} does not equal {expected}")
        require(integer(row, "object_count_failures") == 0,
                f"run {run_id} reports snapshot correctness failures")
        require(integer(row, "disconnects") == 0 and integer(row, "traffic_match") == 1,
                f"run {run_id} reports a disconnect or traffic mismatch")
        require(integer(row, "success") == 1 and not row.get("error", ""),
                f"run {run_id} failed: {row.get('error', '')}")
        require(integer(row, "baseline_mismatches") == 0,
                f"run {run_id} reports an unexplained baseline mismatch")
        require(integer(row, "baseline_bytes") > 0,
                f"run {run_id} has no baseline estimate")
        full = integer(row, "full_snapshots")
        delta = integer(row, "delta_snapshots")
        resync = integer(row, "resync_requests")
        expected_resync = (clients * ((args.iterations - 1) // args.resync_every)
                           if args.resync_every > 0 and cell[0] == "delta" else 0)
        require(full + delta == expected, f"run {run_id} has incomplete snapshot telemetry")
        require(resync == expected_resync, f"run {run_id} has {resync} resyncs; expected {expected_resync}")
        if cell[0] == "full":
            require(full == expected and delta == 0, f"run {run_id} did not stay full-snapshot")
        else:
            require(full == expected_resync and delta == expected - expected_resync,
                    f"run {run_id} delta/full response counts are wrong")
        require(number(row, "elapsed_seconds") > 0 and
                number(row, "throughput_round_trips_per_second") > 0,
                f"run {run_id} has invalid timing")
        seen.add(cell)
        by_id[run_id] = row
    require(seen == expected_cells, "runs.csv does not contain the complete matrix")
    require(set(by_id) == set(range(1, len(rows) + 1)), "run IDs must be contiguous")
    return by_id


def validate_latency(rows: list[dict[str, str]], runs: dict[int, dict[str, str]],
                     args: argparse.Namespace) -> dict[int, list[float]]:
    expected_total = (len(STRATEGIES) * len(PLATFORMS) * len(DATA_BYTES) *
                      args.repetitions * sum(CLIENTS) * args.iterations)
    require(len(rows) == expected_total,
            f"latency_samples.csv has {len(rows)} rows; expected {expected_total}")
    samples: dict[int, list[float]] = defaultdict(list)
    seen: set[tuple[int, int, int]] = set()
    for row in rows:
        run_id = integer(row, "run_id")
        require(run_id in runs, f"sample references unknown run {run_id}")
        run = runs[run_id]
        for field in ("format", "clients", "platforms", "repetition",
                      "snapshot_strategy", "player_data_bytes"):
            require(row.get(field) == run.get(field),
                    f"sample metadata differs from run {run_id} for {field}")
        client_index = integer(row, "client_index")
        iteration = integer(row, "iteration")
        key = (run_id, client_index, iteration)
        require(key not in seen, f"duplicate latency sample {key}")
        require(0 <= client_index < integer(run, "clients") and
                0 <= iteration < args.iterations, f"invalid sample index {key}")
        seen.add(key)
        samples[run_id].append(integer(row, "rtt_ns") / 1_000_000.0)
    for run_id, run in runs.items():
        require(len(samples[run_id]) == integer(run, "clients") * args.iterations,
                f"run {run_id} has the wrong sample count")
    return samples


def summarize(runs: dict[int, dict[str, str]], samples: dict[int, list[float]],
              args: argparse.Namespace) -> list[dict[str, float | int | str]]:
    grouped: dict[tuple[str, int, int, int], list[tuple[int, dict[str, str]]]] = defaultdict(list)
    for run_id, row in runs.items():
        key = (row["snapshot_strategy"], integer(row, "clients"),
               integer(row, "platforms"), integer(row, "player_data_bytes"))
        grouped[key].append((run_id, row))

    output: list[dict[str, float | int | str]] = []
    for clients in CLIENTS:
        for platforms in PLATFORMS:
            for data_bytes in DATA_BYTES:
                for strategy in STRATEGIES:
                    key = (strategy, clients, platforms, data_bytes)
                    entries = grouped[key]
                    require(len(entries) == args.repetitions,
                            f"summary cell {key} has {len(entries)} runs")
                    elapsed = [number(row, "elapsed_seconds") for _, row in entries]
                    achieved = [number(row, "throughput_round_trips_per_second") /
                                integer(row, "clients") for _, row in entries]
                    payload = [(integer(row, "payload_bytes_sent") +
                                integer(row, "payload_bytes_received")) /
                               integer(row, "completed_round_trips") for _, row in entries]
                    baseline = [integer(row, "baseline_bytes") / integer(row, "clients")
                                for _, row in entries]
                    pooled = [value for run_id, _ in entries for value in samples[run_id]]
                    elapsed_stats = mean_variance(elapsed)
                    achieved_stats = mean_variance(achieved)
                    payload_stats = mean_variance(payload)
                    baseline_stats = mean_variance(baseline)
                    output.append({
                        "snapshot_strategy": strategy,
                        "clients": clients,
                        "platforms": platforms,
                        "player_data_bytes": data_bytes,
                        "runs": len(entries),
                        "rtt_samples": len(pooled),
                        "elapsed_mean_seconds": elapsed_stats[0],
                        "elapsed_sample_stddev": elapsed_stats[2],
                        "achieved_hz_per_client_mean": achieved_stats[0],
                        "achieved_hz_per_client_sample_stddev": achieved_stats[2],
                        "payload_bytes_per_update_mean": payload_stats[0],
                        "payload_bytes_per_update_sample_stddev": payload_stats[2],
                        "baseline_bytes_per_client_mean": baseline_stats[0],
                        "baseline_bytes_per_client_sample_stddev": baseline_stats[2],
                        "rtt_mean_ms": statistics.fmean(pooled),
                        "rtt_p50_ms": percentile(pooled, 0.50),
                        "rtt_p95_ms": percentile(pooled, 0.95),
                        "rtt_p99_ms": percentile(pooled, 0.99),
                        "delta_payload_reduction_fraction": "",
                        "delta_over_full_rtt_ratio": "",
                    })

    by_key = {(row["snapshot_strategy"], row["clients"], row["platforms"],
               row["player_data_bytes"]): row for row in output}
    for clients in CLIENTS:
        for platforms in PLATFORMS:
            for data_bytes in DATA_BYTES:
                full = by_key[("full", clients, platforms, data_bytes)]
                delta = by_key[("delta", clients, platforms, data_bytes)]
                reduction = 1.0 - (float(delta["payload_bytes_per_update_mean"]) /
                                   float(full["payload_bytes_per_update_mean"]))
                ratio = float(delta["rtt_mean_ms"]) / float(full["rtt_mean_ms"])
                for row in (full, delta):
                    row["delta_payload_reduction_fraction"] = reduction
                    row["delta_over_full_rtt_ratio"] = ratio
    return output


def write_outputs(result_dir: Path, rows: list[dict[str, float | int | str]]) -> None:
    with (result_dir / "summary.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    by_key = {(row["snapshot_strategy"], row["clients"], row["platforms"],
               row["player_data_bytes"]): row for row in rows}
    cells = [(clients, platforms, data) for clients in CLIENTS
             for platforms in PLATFORMS for data in DATA_BYTES]
    labels = [f"{clients}c {platforms}p {data}B" for clients, platforms, data in cells]
    colors = {"full": "#4C78A8", "delta": "#F58518"}

    def values(strategy: str, field: str) -> list[float]:
        return [float(by_key[(strategy, clients, platforms, data)][field])
                for clients, platforms, data in cells]

    grouped_bar_svg(result_dir / "snapshot_payload.svg", "Binary snapshot payload at equal load",
                    "bytes / update", labels,
                    [(name, values(name, "payload_bytes_per_update_mean"), colors[name])
                     for name in STRATEGIES])
    grouped_bar_svg(result_dir / "snapshot_rtt.svg", "Binary snapshot RTT at equal load",
                    "mean RTT (ms)", labels,
                    [(name, values(name, "rtt_mean_ms"), colors[name]) for name in STRATEGIES])
    grouped_bar_svg(result_dir / "snapshot_rate.svg", "Achieved update rate per client",
                    "updates / second", labels,
                    [(name, values(name, "achieved_hz_per_client_mean"), colors[name])
                     for name in STRATEGIES], reference=60.0)
    reductions = [float(by_key[("delta", clients, platforms, data)]
                        ["delta_payload_reduction_fraction"]) * 100.0
                  for clients, platforms, data in cells]
    grouped_bar_svg(result_dir / "snapshot_payload_reduction.svg",
                    "Property-delta payload reduction", "reduction (%)", labels,
                    [("delta reduction", reductions, "#54A24B")])


def main() -> int:
    args = parse_args()
    require(args.repetitions > 0 and args.warmup > 0 and args.iterations > 0 and
            args.offered_hz > 0 and args.resync_every >= 0, "expected values are invalid")
    runs = validate_runs(load_csv(args.result_dir / "runs.csv"), args)
    samples = validate_latency(load_csv(args.result_dir / "latency_samples.csv"), runs, args)
    rows = summarize(runs, samples, args)
    write_outputs(args.result_dir, rows)
    print(f"validated {len(runs)} runs and {sum(map(len, samples.values()))} RTT samples")
    print(f"wrote {len(rows)} summary rows and 4 SVGs to {args.result_dir}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"analysis failed: {error}", file=sys.stderr)
        sys.exit(1)
