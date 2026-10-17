#!/usr/bin/env python3
"""Validate canonical network benchmark CSVs and generate summaries/SVGs."""

from __future__ import annotations

import argparse
import csv
import html
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


FORMATS = ("text", "binary")
CLIENTS = (1, 2, 4)
PLATFORMS = (10, 100, 500)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_dir", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--iterations", type=int, default=1000)
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def integer(row: dict[str, str], field: str) -> int:
    try:
        return int(row[field])
    except (KeyError, ValueError) as error:
        raise ValueError(f"invalid integer field {field!r} in row {row}") from error


def number(row: dict[str, str], field: str) -> float:
    try:
        value = float(row[field])
    except (KeyError, ValueError) as error:
        raise ValueError(f"invalid numeric field {field!r} in row {row}") from error
    require(math.isfinite(value), f"non-finite {field!r} in row {row}")
    return value


def load_csv(path: Path) -> list[dict[str, str]]:
    require(path.is_file(), f"missing required input: {path}")
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        require(reader.fieldnames is not None, f"{path} has no header")
        return list(reader)


def percentile(values: list[float], probability: float) -> float:
    require(values, "cannot calculate a percentile of no samples")
    ordered = sorted(values)
    position = (len(ordered) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def mean_variance(values: list[float]) -> tuple[float, float, float]:
    require(values, "cannot summarize no values")
    mean = statistics.fmean(values)
    variance = statistics.variance(values) if len(values) > 1 else 0.0
    return mean, variance, math.sqrt(variance)


def validate_runs(rows: list[dict[str, str]], args: argparse.Namespace) -> dict[int, dict[str, str]]:
    expected_cells = {
        (format_name, clients, platforms, repetition)
        for format_name in FORMATS
        for clients in CLIENTS
        for platforms in PLATFORMS
        for repetition in range(1, args.repetitions + 1)
    }
    require(len(rows) == len(expected_cells),
            f"runs.csv has {len(rows)} rows; expected {len(expected_cells)}")

    by_id: dict[int, dict[str, str]] = {}
    seen_cells: set[tuple[str, int, int, int]] = set()
    for row in rows:
        run_id = integer(row, "run_id")
        require(run_id not in by_id, f"duplicate run_id {run_id}")
        by_id[run_id] = row

        format_name = row.get("format", "")
        clients = integer(row, "clients")
        platforms = integer(row, "platforms")
        repetition = integer(row, "repetition")
        cell = (format_name, clients, platforms, repetition)
        require(cell in expected_cells, f"unexpected matrix cell {cell}")
        require(cell not in seen_cells, f"duplicate matrix cell {cell}")
        seen_cells.add(cell)

        require(integer(row, "warmup_iterations") == args.warmup,
                f"run {run_id} has the wrong warm-up count")
        require(integer(row, "measured_iterations") == args.iterations,
                f"run {run_id} has the wrong measured count")
        expected = clients * args.iterations
        require(integer(row, "expected_position_updates") == expected,
                f"run {run_id} has the wrong expected update count")
        for field in ("completed_round_trips", "rtt_sample_count", "messages_sent",
                      "messages_received", "object_count_checks", "server_position_updates"):
            require(integer(row, field) == expected,
                    f"run {run_id} field {field} does not equal {expected}")
        require(integer(row, "object_count_failures") == 0,
                f"run {run_id} reports object-count failures")
        require(integer(row, "disconnects") == 0, f"run {run_id} reports disconnects")
        require(integer(row, "traffic_match") == 1, f"run {run_id} traffic did not match")
        require(integer(row, "success") == 1, f"run {run_id} was unsuccessful: {row.get('error', '')}")
        require(not row.get("error", ""), f"run {run_id} has an error message")
        require(number(row, "elapsed_seconds") > 0.0, f"run {run_id} has non-positive elapsed time")
        require(number(row, "throughput_round_trips_per_second") > 0.0,
                f"run {run_id} has non-positive throughput")
        require(integer(row, "payload_bytes_sent") > 0 and integer(row, "payload_bytes_received") > 0,
                f"run {run_id} has no payload bytes")
        if format_name == "binary":
            require(integer(row, "frames_sent") == expected and integer(row, "frames_received") == expected,
                    f"binary run {run_id} did not use one frame per message")
        else:
            require(integer(row, "frames_sent") > expected and integer(row, "frames_received") > expected,
                    f"text run {run_id} did not use multipart messages")

    require(seen_cells == expected_cells, "runs.csv does not contain the complete matrix")
    require(set(by_id) == set(range(1, len(rows) + 1)), "run IDs must be contiguous starting at 1")
    return by_id


def validate_latency(rows: list[dict[str, str]], runs: dict[int, dict[str, str]],
                     args: argparse.Namespace) -> dict[int, list[float]]:
    expected_total = len(FORMATS) * len(PLATFORMS) * args.repetitions * sum(CLIENTS) * args.iterations
    require(len(rows) == expected_total,
            f"latency_samples.csv has {len(rows)} rows; expected {expected_total}")

    samples: dict[int, list[float]] = defaultdict(list)
    seen: set[tuple[int, int, int]] = set()
    for row in rows:
        run_id = integer(row, "run_id")
        require(run_id in runs, f"latency sample references unknown run {run_id}")
        run = runs[run_id]
        for field in ("format", "clients", "platforms", "repetition"):
            require(row.get(field) == run.get(field),
                    f"latency metadata differs from run {run_id} for {field}")
        client_index = integer(row, "client_index")
        iteration = integer(row, "iteration")
        require(0 <= client_index < integer(run, "clients"),
                f"invalid client index in run {run_id}")
        require(0 <= iteration < args.iterations, f"invalid iteration in run {run_id}")
        key = (run_id, client_index, iteration)
        require(key not in seen, f"duplicate latency sample {key}")
        seen.add(key)
        rtt_ns = integer(row, "rtt_ns")
        require(rtt_ns >= 0, f"negative RTT in run {run_id}")
        samples[run_id].append(rtt_ns / 1_000_000.0)

    for run_id, run in runs.items():
        expected = integer(run, "clients") * args.iterations
        require(len(samples[run_id]) == expected,
                f"run {run_id} has {len(samples[run_id])} RTT samples; expected {expected}")
    return samples


def summarize(runs: dict[int, dict[str, str]], samples: dict[int, list[float]],
              args: argparse.Namespace) -> list[dict[str, float | int | str]]:
    grouped_runs: dict[tuple[str, int, int], list[dict[str, str]]] = defaultdict(list)
    grouped_rtt: dict[tuple[str, int, int], list[float]] = defaultdict(list)
    for run_id, row in runs.items():
        key = (row["format"], integer(row, "clients"), integer(row, "platforms"))
        grouped_runs[key].append(row)
        grouped_rtt[key].extend(samples[run_id])

    summaries: list[dict[str, float | int | str]] = []
    for clients in CLIENTS:
        for platforms in PLATFORMS:
            for format_name in FORMATS:
                key = (format_name, clients, platforms)
                rows = grouped_runs[key]
                require(len(rows) == args.repetitions,
                        f"summary cell {key} has {len(rows)} runs; expected {args.repetitions}")
                elapsed = [number(row, "elapsed_seconds") for row in rows]
                throughput = [number(row, "throughput_round_trips_per_second") for row in rows]
                bytes_per_iteration = [
                    (integer(row, "payload_bytes_sent") + integer(row, "payload_bytes_received")) /
                    integer(row, "completed_round_trips")
                    for row in rows
                ]
                frames_per_iteration = [
                    (integer(row, "frames_sent") + integer(row, "frames_received")) /
                    integer(row, "completed_round_trips")
                    for row in rows
                ]
                elapsed_stats = mean_variance(elapsed)
                throughput_stats = mean_variance(throughput)
                byte_stats = mean_variance(bytes_per_iteration)
                frame_stats = mean_variance(frames_per_iteration)
                rtt = grouped_rtt[key]
                summaries.append({
                    "format": format_name,
                    "clients": clients,
                    "platforms": platforms,
                    "runs": len(rows),
                    "rtt_samples": len(rtt),
                    "elapsed_mean_seconds": elapsed_stats[0],
                    "elapsed_sample_variance": elapsed_stats[1],
                    "elapsed_sample_stddev": elapsed_stats[2],
                    "throughput_mean_round_trips_per_second": throughput_stats[0],
                    "throughput_sample_variance": throughput_stats[1],
                    "throughput_sample_stddev": throughput_stats[2],
                    "payload_bytes_per_iteration_mean": byte_stats[0],
                    "payload_bytes_per_iteration_sample_variance": byte_stats[1],
                    "payload_bytes_per_iteration_sample_stddev": byte_stats[2],
                    "frames_per_iteration_mean": frame_stats[0],
                    "frames_per_iteration_sample_variance": frame_stats[1],
                    "frames_per_iteration_sample_stddev": frame_stats[2],
                    "rtt_mean_ms": statistics.fmean(rtt),
                    "rtt_p50_ms": percentile(rtt, 0.50),
                    "rtt_p95_ms": percentile(rtt, 0.95),
                    "rtt_p99_ms": percentile(rtt, 0.99),
                    "binary_elapsed_speedup": "",
                    "binary_throughput_speedup": "",
                    "binary_payload_reduction_fraction": "",
                })

    by_key = {(row["format"], row["clients"], row["platforms"]): row for row in summaries}
    for clients in CLIENTS:
        for platforms in PLATFORMS:
            text = by_key[("text", clients, platforms)]
            binary = by_key[("binary", clients, platforms)]
            elapsed_speedup = float(text["elapsed_mean_seconds"]) / float(binary["elapsed_mean_seconds"])
            throughput_speedup = (float(binary["throughput_mean_round_trips_per_second"]) /
                                  float(text["throughput_mean_round_trips_per_second"]))
            payload_reduction = 1.0 - (float(binary["payload_bytes_per_iteration_mean"]) /
                                       float(text["payload_bytes_per_iteration_mean"]))
            for row in (text, binary):
                row["binary_elapsed_speedup"] = elapsed_speedup
                row["binary_throughput_speedup"] = throughput_speedup
                row["binary_payload_reduction_fraction"] = payload_reduction
    return summaries


def write_summary(path: Path, rows: list[dict[str, float | int | str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def grouped_bar_svg(path: Path, title: str, unit: str, labels: list[str],
                    series: list[tuple[str, list[float], str]], reference: float | None = None) -> None:
    width, height = 1280, 700
    left, right, top, bottom = 100, 40, 80, 150
    plot_width, plot_height = width - left - right, height - top - bottom
    maximum = max(max(values) for _, values, _ in series)
    if reference is not None:
        maximum = max(maximum, reference)
    maximum = maximum * 1.12 if maximum > 0 else 1.0
    group_width = plot_width / len(labels)
    bar_width = group_width * 0.68 / len(series)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<text x="{width / 2}" y="36" text-anchor="middle" font-family="sans-serif" font-size="24" font-weight="bold">{html.escape(title)}</text>',
        f'<text x="24" y="{top + plot_height / 2}" text-anchor="middle" transform="rotate(-90 24 {top + plot_height / 2})" font-family="sans-serif" font-size="15">{html.escape(unit)}</text>',
    ]
    for tick in range(6):
        value = maximum * tick / 5
        y = top + plot_height - plot_height * tick / 5
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_width}" y2="{y:.2f}" stroke="#dddddd"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 5:.2f}" text-anchor="end" font-family="sans-serif" font-size="12">{value:.3g}</text>')
    if reference is not None:
        y = top + plot_height - reference / maximum * plot_height
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_width}" y2="{y:.2f}" stroke="#555" stroke-dasharray="7 5"/>')
        parts.append(f'<text x="{left + plot_width - 5}" y="{y - 7:.2f}" text-anchor="end" font-family="sans-serif" font-size="12">1× parity</text>')
    for group, label in enumerate(labels):
        center = left + (group + 0.5) * group_width
        start = center - bar_width * len(series) / 2
        for series_index, (_, values, color) in enumerate(series):
            value = values[group]
            bar_height = value / maximum * plot_height
            x = start + series_index * bar_width
            y = top + plot_height - bar_height
            parts.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_width * 0.88:.2f}" height="{bar_height:.2f}" fill="{color}"/>')
        parts.append(f'<text x="{center:.2f}" y="{top + plot_height + 24}" text-anchor="end" transform="rotate(-42 {center:.2f} {top + plot_height + 24})" font-family="sans-serif" font-size="13">{html.escape(label)}</text>')
    legend_x = left
    for name, _, color in series:
        parts.append(f'<rect x="{legend_x}" y="{height - 34}" width="18" height="18" fill="{color}"/>')
        parts.append(f'<text x="{legend_x + 25}" y="{height - 20}" font-family="sans-serif" font-size="14">{html.escape(name)}</text>')
        legend_x += 150
    parts.append('</svg>')
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def write_graphs(result_dir: Path, summaries: list[dict[str, float | int | str]]) -> None:
    by_key = {(row["format"], row["clients"], row["platforms"]): row for row in summaries}
    cells = [(clients, platforms) for clients in CLIENTS for platforms in PLATFORMS]
    labels = [f"{clients} client{'s' if clients != 1 else ''}, {platforms} platforms"
              for clients, platforms in cells]
    colors = {"text": "#4C78A8", "binary": "#F58518"}

    def values(format_name: str, field: str) -> list[float]:
        return [float(by_key[(format_name, clients, platforms)][field]) for clients, platforms in cells]

    grouped_bar_svg(
        result_dir / "elapsed_time.svg", "Mean elapsed time by matrix cell", "seconds",
        labels, [(name, values(name, "elapsed_mean_seconds"), colors[name]) for name in FORMATS])
    grouped_bar_svg(
        result_dir / "throughput.svg", "Mean aggregate throughput", "round trips / second",
        labels, [(name, values(name, "throughput_mean_round_trips_per_second"), colors[name])
                 for name in FORMATS])
    grouped_bar_svg(
        result_dir / "payload_size.svg", "Mean encoded payload per round trip", "bytes / iteration",
        labels, [(name, values(name, "payload_bytes_per_iteration_mean"), colors[name])
                 for name in FORMATS])
    speedups = [float(by_key[("binary", clients, platforms)]["binary_elapsed_speedup"])
                for clients, platforms in cells]
    grouped_bar_svg(
        result_dir / "binary_speedup.svg", "Binary elapsed-time speedup", "text time / binary time",
        labels, [("binary speedup", speedups, "#54A24B")], reference=1.0)


def main() -> int:
    args = parse_args()
    require(args.repetitions > 0 and args.warmup > 0 and args.iterations > 0,
            "expected counts must be positive")
    runs = validate_runs(load_csv(args.result_dir / "runs.csv"), args)
    samples = validate_latency(load_csv(args.result_dir / "latency_samples.csv"), runs, args)
    summaries = summarize(runs, samples, args)
    write_summary(args.result_dir / "summary.csv", summaries)
    write_graphs(args.result_dir, summaries)
    print(f"validated {len(runs)} runs and {sum(len(value) for value in samples.values())} RTT samples")
    print(f"wrote {len(summaries)} summary rows and 4 SVGs to {args.result_dir}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"analysis failed: {error}", file=sys.stderr)
        sys.exit(1)
