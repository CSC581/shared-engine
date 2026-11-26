#!/usr/bin/env python3
"""Collect/validate both Release matrices; requested C++ bytes are not RSS."""
import argparse
import csv
import datetime
import hashlib
import itertools
import json
import math
import platform
import subprocess
import statistics
from pathlib import Path

MODES = ('heap', 'pool', 'reuse')
CAPACITIES = ('32', '64', '128')
WORKLOADS = ('normal', 'burst')
KEYS = ('mode', 'transient_capacity', 'workload')
COMMON = ('accepted', 'rejected', 'objects_constructed', 'components_constructed', 'peak_active', 'modeled_reserved_pool_slot_bytes')
TIMING = ('setup_us', 'update_mean_us', 'update_median_us', 'update_p95_us', 'update_p99_us')
MEMORY = ('allocation_requests', 'requested_bytes', 'live_requested_bytes', 'peak_requested_bytes')


def validate(rows, profile=False, summary=False):
    phases = ('setup', 'steady') if profile else (None,)
    expected = set(itertools.product(MODES, CAPACITIES, WORKLOADS, phases, (None,) if summary else range(1, 6)))
    actual = set()
    metrics = COMMON + (MEMORY if profile else TIMING)
    columns = {*KEYS, *(['phase'] if profile else []), 'runs' if summary else 'repetition'}
    if summary:
        columns.update(f'{m}_{suffix}' for m in metrics for suffix in ('mean', 'stddev'))
    else:
        columns.update(metrics)
    for row in rows:
        if set(row) != columns:
            raise ValueError('CSV columns do not match the measurement schema')
        key = tuple(row[k] for k in KEYS) + (row.get('phase'), None if summary else int(row['repetition']))
        if key not in expected or key in actual:
            raise ValueError(f'Duplicate or unexpected matrix cell: {key}')
        actual.add(key)
        for column, value in row.items():
            if column not in (*KEYS, 'phase'):
                numeric = float(value)
                if not math.isfinite(numeric) or numeric < 0:
                    raise ValueError(f'Invalid metric {column}: {value}')
        if summary and int(row['runs']) != 5:
            raise ValueError('Expected five repetitions in every summary')
    if actual != expected:
        raise ValueError(f'Incomplete matrix: missing {len(expected - actual)} cells')
    if summary:
        return
    work = {}
    for row in rows:
        key = (row['transient_capacity'], row['workload'], row['repetition'])
        if profile and row['phase'] != 'steady':
            continue
        value = (float(row['accepted']), float(row['rejected']))
        if key in work and work[key] != value:
            raise ValueError('Backends accepted or rejected different work')
        work[key] = value
        if row['workload'] == 'normal' and value[1] != 0:
            raise ValueError('Normal workload must fit every capacity')
        if profile and float(row['peak_requested_bytes']) < float(row['live_requested_bytes']):
            raise ValueError('Peak requested bytes below final live requested bytes')


def read_csv(path):
    with path.open(newline='') as source:
        return list(csv.DictReader(source))


def self_test():
    rows = [dict({key: '0' for key in COMMON + TIMING}, mode=m, transient_capacity=c, workload=w, repetition=str(r), accepted='10', rejected='0')
            for m, c, w, r in itertools.product(MODES, CAPACITIES, WORKLOADS, range(1, 6))]
    validate(rows)
    for bad in (rows[:-1], rows + [rows[0]], [dict(rows[0], accepted='nan')] + rows[1:],
                [dict(rows[0], accepted='11')] + rows[1:]):
        try:
            validate(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid matrix accepted')


def command(args, root):
    return subprocess.check_output(args, cwd=root, text=True).strip()


def collect(build, output):
    root = Path(__file__).resolve().parents[4]
    cache_path = build / 'CMakeCache.txt'
    cache = cache_path.read_text()
    settings = {line.split(':', 1)[0]: line.split('=', 1)[1] for line in cache.splitlines()
                if ':' in line and '=' in line and not line.startswith(('#', '//'))}
    flags = {key: value for key, value in settings.items() if key.startswith('CMAKE_CXX_FLAGS')}
    if any('-fsanitize' in value for value in flags.values()):
        raise ValueError('Timing build must not use sanitizer flags')
    if 'CMAKE_BUILD_TYPE:STRING=Release\n' not in cache or 'ENGINE_SANITIZER:STRING=\n' not in cache:
        raise ValueError('Use a Release build without engine sanitizers for timing')
    output.mkdir(parents=True, exist_ok=False)
    game = Path(__file__).resolve().parents[1]
    sources = sorted([*game.rglob('*.cpp'), *game.rglob('*.hpp'), *game.rglob('*.py'),
                      *root.joinpath('src').glob('*.cpp'), *root.joinpath('include').glob('*.hpp'),
                      root / 'benchmarks/BenchmarkSupport.hpp', root / 'CMakeLists.txt', game / 'CMakeLists.txt'])
    metadata = {
        'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'git_commit': command(['git', 'rev-parse', 'HEAD'], root),
        'git_status': command(['git', 'status', '--short'], root),
        'os': platform.platform(), 'cpu': platform.processor(),
        'compiler': command([settings['CMAKE_CXX_COMPILER'], '--version'], root),
        'build_settings': {key: settings[key] for key in ('CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'ENGINE_SANITIZER')},
        'global_flags': flags,
        'flags': {str(p.relative_to(build)): p.read_text() for p in build.glob('individual-games/harsha/neon-siege/CMakeFiles/neon-siege-memory-*/flags.make')},
        'source_sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        'scope': 'Headless lifecycle workload; separate timing and C++ new-request profiling; no RSS or rendered FPS claims.',
    }
    cpuinfo = Path('/proc/cpuinfo')
    if cpuinfo.exists():
        metadata['cpu_models'] = sorted({line.split(':', 1)[1].strip() for line in cpuinfo.read_text().splitlines() if line.startswith('model name')})
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    tables = {}
    for kind in ('benchmark', 'profile'):
        argv = [str(build / f'neon-siege-memory-{kind}'), '--runs', str(output / f'{kind}-runs.csv'), '--summary', str(output / f'{kind}-summary.csv')]
        with (output / f'{kind}.log').open('w') as log:
            subprocess.run(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        for summary in (False, True):
            rows = read_csv(output / f'{kind}-{"summary" if summary else "runs"}.csv')
            validate(rows, profile=kind == 'profile', summary=summary)
            tables[kind, summary] = rows
        for summary_row in tables[kind, True]:
            matching = [row for row in tables[kind, False] if all(row[key] == summary_row[key] for key in (*KEYS, *(['phase'] if kind == 'profile' else []))) ]
            for metric in COMMON + (MEMORY if kind == 'profile' else TIMING):
                values = [float(row[metric]) for row in matching]
                for suffix, expected in (('mean', statistics.fmean(values)), ('stddev', statistics.stdev(values))):
                    if not math.isclose(float(summary_row[f'{metric}_{suffix}']), expected, rel_tol=1e-6, abs_tol=1e-7):
                        raise ValueError(f'Summary disagrees with raw measurements: {metric}_{suffix}')
    timing = {tuple(row[k] for k in KEYS) + (row['repetition'],): row for row in tables['benchmark', False]}
    for row in tables['profile', False]:
        if row['phase'] != 'steady':
            continue
        paired = timing[tuple(row[k] for k in KEYS) + (row['repetition'],)]
        for field in ('accepted', 'rejected', 'objects_constructed', 'components_constructed'):
            if float(row[field]) != float(paired[field]):
                raise ValueError(f'Profiling and timing work differs in {field}')
    profile = {tuple(row[k] for k in KEYS): row for row in tables['profile', True] if row['phase'] == 'steady'}
    report = ['# Neon Siege memory study', '',
              'Five Release repetitions per condition. p95 is the mean of the five per-run p95 values. '
              'Bytes are peak outstanding requested C++ bytes, including persistent setup allocations; they exclude profiler headers, malloc overhead, C allocations and GPU memory. '
              'Different capacities can reject different amounts of work: compare modes within a capacity/workload.', '',
              '| Capacity | Workload | Mode | Update p95 (us) | C++ requests | Peak requested bytes | Rejected |',
              '| --- | --- | --- | --- | --- | --- | --- |']
    for row in tables['benchmark', True]:
        memory = profile[tuple(row[k] for k in KEYS)]
        report.append(f"| {row['transient_capacity']} | {row['workload']} | {row['mode']} | {float(row['update_p95_us_mean']):.2f} | {float(memory['allocation_requests_mean']):.0f} | {float(memory['peak_requested_bytes_mean']):.0f} | {float(row['rejected_mean']):.0f} |")
    report.extend(['', 'The workload uses game factories and engine motion/collision against twelve persistent targets. '
                   'It is a lifecycle experiment, not a complete-match or render benchmark. '
                   'Reuse still scans inactive slots; engine scratch containers still allocate. Heap and pooled World also differ in ownership-vector reservation.'])
    (output / 'report.md').write_text('\n'.join(report) + '\n')
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-release'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    self_test()
    if not args.self_test:
        destination = args.output or Path('build/neon-memory-results') / datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        collect(args.build.resolve(), destination.resolve())
