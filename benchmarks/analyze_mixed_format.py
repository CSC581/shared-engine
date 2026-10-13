#!/usr/bin/env python3
"""Compare each format's RTT alone vs sharing a server with the other format.

Input is the output of `network-performance-benchmark --mixed`. For every
client count and platform count, a format's "alone" RTT comes from the
all-text or all-binary run and its "mixed" RTT from the clients of that format
in the half-text/half-binary run. Both have the same total number of clients,
so the only change is which format the other clients use.
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

from analyze_network_performance import (
    PLATFORMS,
    grouped_bar_svg,
    integer,
    load_csv,
    mean_variance,
    percentile,
    require,
)

COMPOSITIONS = ("text", "binary", "mixed")
CLIENT_FORMATS = ("text", "binary")
CLIENTS = (2, 4)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_dir", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--iterations", type=int, default=1000)
    return parser.parse_args()


def validate_runs(rows: list[dict[str, str]], args: argparse.Namespace) -> dict[int, dict[str, str]]:
    expected_cells = {
        (composition, clients, platforms, repetition)
        for composition in COMPOSITIONS
        for clients in CLIENTS
        for platforms in PLATFORMS
        for repetition in range(1, args.repetitions + 1)
    }
    require(len(rows) == len(expected_cells),
            f"runs.csv has {len(rows)} rows; expected {len(expected_cells)}")
    by_id: dict[int, dict[str, str]] = {}
    seen: set[tuple[str, int, int, int]] = set()
    for row in rows:
        run_id = integer(row, "run_id")
        cell = (row.get("format", ""), integer(row, "clients"), integer(row, "platforms"),
                integer(row, "repetition"))
        require(cell in expected_cells and cell not in seen, f"unexpected or duplicate cell {cell}")
        seen.add(cell)
        require(integer(row, "measured_iterations") == args.iterations,
                f"run {run_id} has the wrong measured count")
        require(integer(row, "success") == 1, f"run {run_id} was unsuccessful: {row.get('error', '')}")
        by_id[run_id] = row
    return by_id


def collect_samples(rows: list[dict[str, str]], runs: dict[int, dict[str, str]],
                    args: argparse.Namespace) -> dict[tuple[int, str], list[float]]:
    """RTT samples in milliseconds, keyed by run and the client's own format."""
    samples: dict[tuple[int, str], list[float]] = defaultdict(list)
    for row in rows:
        run_id = integer(row, "run_id")
        require(run_id in runs, f"latency sample references unknown run {run_id}")
        composition = runs[run_id]["format"]
        client_format = row.get("client_format", "")
        require(client_format in CLIENT_FORMATS, f"run {run_id} has client_format {client_format!r}")
        require(composition == "mixed" or client_format == composition,
                f"run {run_id} is {composition} but has a {client_format} client")
        samples[(run_id, client_format)].append(integer(row, "rtt_ns") / 1_000_000.0)

    for run_id, run in runs.items():
        clients = integer(run, "clients")
        if run["format"] == "mixed":
            for client_format in CLIENT_FORMATS:
                expected = clients // 2 * args.iterations
                require(len(samples[(run_id, client_format)]) == expected,
                        f"mixed run {run_id} has the wrong {client_format} sample count")
        else:
            require(len(samples[(run_id, run["format"])]) == clients * args.iterations,
                    f"run {run_id} has the wrong sample count")
    return samples


def summarize(runs: dict[int, dict[str, str]], samples: dict[tuple[int, str], list[float]],
              args: argparse.Namespace) -> list[dict[str, float | int | str]]:
    run_ids: dict[tuple[str, int, int], list[int]] = defaultdict(list)
    for run_id, row in runs.items():
        run_ids[(row["format"], integer(row, "clients"), integer(row, "platforms"))].append(run_id)

    summaries: list[dict[str, float | int | str]] = []
    for clients in CLIENTS:
        for platforms in PLATFORMS:
            for client_format in CLIENT_FORMATS:
                rows = {}
                for setting, composition in (("alone", client_format), ("mixed", "mixed")):
                    ids = run_ids[(composition, clients, platforms)]
                    require(len(ids) == args.repetitions,
                            f"{composition}/{clients}/{platforms} has {len(ids)} runs")
                    per_run = [statistics.fmean(samples[(run_id, client_format)]) for run_id in ids]
                    pooled = [value for run_id in ids for value in samples[(run_id, client_format)]]
                    mean, variance, stddev = mean_variance(per_run)
                    rows[setting] = {
                        "client_format": client_format,
                        "setting": setting,
                        "clients": clients,
                        "platforms": platforms,
                        "runs": len(ids),
                        "rtt_samples": len(pooled),
                        "rtt_mean_ms": mean,
                        "rtt_run_mean_sample_variance": variance,
                        "rtt_run_mean_sample_stddev": stddev,
                        "rtt_p50_ms": percentile(pooled, 0.50),
                        "rtt_p95_ms": percentile(pooled, 0.95),
                        "mixed_over_alone_ratio": "",
                    }
                ratio = float(rows["mixed"]["rtt_mean_ms"]) / float(rows["alone"]["rtt_mean_ms"])
                for row in rows.values():
                    row["mixed_over_alone_ratio"] = ratio
                summaries.extend((rows["alone"], rows["mixed"]))
    return summaries


def write_outputs(result_dir: Path, summaries: list[dict[str, float | int | str]]) -> None:
    with (result_dir / "mixed_summary.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(summaries[0].keys()))
        writer.writeheader()
        writer.writerows(summaries)

    by_key = {(row["client_format"], row["setting"], row["clients"], row["platforms"]): row
              for row in summaries}
    cells = [(clients, platforms) for clients in CLIENTS for platforms in PLATFORMS]
    labels = [f"{clients} clients, {platforms} platforms" for clients, platforms in cells]
    colors = {"alone": "#4C78A8", "mixed": "#F58518"}
    # One graph per format: text RTT is tens of times larger, so a shared axis
    # would flatten the binary bars.
    for client_format in CLIENT_FORMATS:
        series = [
            (f"{client_format} {setting}",
             [float(by_key[(client_format, setting, clients, platforms)]["rtt_mean_ms"])
              for clients, platforms in cells],
             colors[setting])
            for setting in ("alone", "mixed")
        ]
        grouped_bar_svg(result_dir / f"mixed_{client_format}_rtt.svg",
                        f"{client_format.capitalize()} client RTT, alone vs mixed", "mean RTT (ms)",
                        labels, series)


def main() -> int:
    args = parse_args()
    require(args.repetitions > 0 and args.iterations > 0, "expected counts must be positive")
    runs = validate_runs(load_csv(args.result_dir / "runs.csv"), args)
    samples = collect_samples(load_csv(args.result_dir / "latency_samples.csv"), runs, args)
    summaries = summarize(runs, samples, args)
    write_outputs(args.result_dir, summaries)
    for row in summaries:
        if row["setting"] == "mixed":
            print(f"{row['client_format']:>6} {row['clients']} clients {row['platforms']:>3} platforms: "
                  f"mixed/alone RTT = {float(row['mixed_over_alone_ratio']):.3f}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"analysis failed: {error}", file=sys.stderr)
        sys.exit(1)
