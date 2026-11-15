# Neon Siege

A single-player neon wave shooter built on the existing engine. Clear two
formations, then defeat a forty-health boss. The boss cycles through straight,
spread and spiral attacks. Lose all three lives, or let a formation reach the
horizontal defensive line, and the match ends.

## Build and play

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target neon-siege -j 4
./build/neon-siege
```

| Input | Action |
| --- | --- |
| A / D or Left / Right | Move horizontally |
| Space, held | Fire |
| A + D or Left + Right | Shield for 0.35 seconds; three-second cooldown |
| P | Pause / resume |
| R | Restart with fresh lives, timers and objects |
| Escape | Quit |

The shield uses movement keys that already act independently. Both directions
cancel movement. Holding the chord does not activate another shield when its
cooldown finishes: release and press it again. Damage grants one second of
invulnerability. Pause and focus loss clear held input.

Ships, stars, HUD and effects use SDL primitives, with no asset downloads.
Gameplay updates at 240 fixed steps/second using engine motion and timing.
Enemy formations use the engine's `PathMover` and descend at reversals.

## What is reused

| Engine system | Game use |
| --- | --- |
| `Engine`, `Game`, proportional rendering | Window, frame loop, resize and renderer ownership |
| `World`, `GameObject`, components | Actor identity, ownership, activation and lifetime |
| `World(capacity)`, custom allocator | Fixed object storage; no heap fallback when full |
| `Transform`, `Motion`, `FrameTime` | Player and projectile movement |
| `PathMover` | Hidden horizontal formation guide |
| `Gravity` | Cosmetic particle motion |
| `Collider`, World collision pass | Trigger contacts and deferred hit candidates |
| `Renderable`, `renderWorld()` | Actors, bullets, particles and render layers |
| Input events and `registerChord` | Movement, fire, shield, pause and restart |
| Game timeline | Pause and time progression |

The engine source and public headers are unchanged. Recipes/terrain remain in
Frostbrew; Neon Siege owns its formation, damage, attack and match rules.
Ordinary implementations live in cpp files, with declarations in headers.

Default storage reserves 237 GameObjects: one player, one guide, 18 enemies,
one boss, 24 player bullets, 128 enemy bullets and 64 particles. All reusable
actors and their components are constructed before play. Inactive actors still
occupy storage. Cosmetic slots cannot starve projectiles. Full categories skip
new spawns; a rejected player shot consumes its normal fire interval.

Retirement is deferred until the next simulation step. Each activation has a
generation, and hit records validate that generation. Projectile hits are
applied once, with hostile hits first so simultaneous final kills cause defeat.
`onCollide` handles reused IDs even without a fresh collision-enter event.
The formation guide is reconstructed between waves because `PathMover` does
not expose a reset API. Restart reconstructs the entire arena.

## Memory study

The same game-local factories/storage support three bounded backends:

| Mode | What happens when an actor expires |
| --- | --- |
| `heap` | Destroy it; construct the next actor in an ordinary World |
| `pool` | Destroy it; reconstruct the next actor in fixed GameObject storage |
| `reuse` | Deactivate it; reset the existing object and components next time |

Pooling GameObject storage alone leaves component construction allocations.
Complete reuse avoids those repeated constructions, but reserves more memory
and still scans inactive component entries. Engine update/collision scratch
containers continue allocating. None of the modes promises allocation-free
frames or automatically lower total memory use.

Build uninstrumented Release targets and collect both full matrices:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target neon-siege-memory-benchmark neon-siege-memory-profile -j 4
python3 individual-games/harsha/neon-siege/benchmarks/collect_study.py --build build-release
```

The collector creates a new timestamped folder under `build/neon-memory-results/`
containing raw CSVs, summaries, separate logs, source hashes, compiler/build and
machine metadata, and `report.md`. It fails on incomplete matrices, unequal
accepted/rejected work, non-finite metrics, or summaries that disagree with raw
measurements. A failed collection preserves its diagnostics.

Each executable checks per-step parity, then verifies full-trial outcomes
across modes and repetitions. Full runs use five repetitions with rotating
backend order, 240 warm-up updates and 2,400 measured updates. Capacities are
32/64/128 transient actors in addition to twelve persistent targets. Normal
births fit every capacity; burst births deliberately exhaust slots. Fixed
seeds, quotas and inputs are identical within each condition. The targets
remain alive so the lifecycle workload continues after repeated contacts.
This is a subsystem experiment, not a full-match or rendered-FPS benchmark.

The **timing executable has no allocation hooks**. The separate profiling
executable counts successful C++ `new` requests, including scalar, array and
aligned forms. Setup and steady operation have separate counts. Peak live
requested bytes include persistent setup/workload bookkeeping, not allocator
headers, allocator overhead, direct C allocations, GPU allocations or RSS.
`modeled_reserved_pool_slot_bytes` is only `pool_capacity * sizeof(GameObject)`;
heap mode reserves no pool slots, so that column is zero for heap mode.

A local five-run Release collection using game/benchmark source at `37209ac`
is preserved in `build/neon-memory-study-final/`. Its normal-load, capacity-32
results were:

| Mode | Mean per-run update p95 (µs) | Steady C++ requests | Peak live requested bytes |
| --- | --- | --- | --- |
| heap | 3.81 | 17,600 | 17,056 |
| pool | 3.78 | 17,440 | 20,116 |
| reuse | 2.28 | 15,280 | 29,908 |

Here complete reuse reduced update tails while consuming more requested
memory. At capacity 128, normal-load reuse had a p95 of 3.17 µs and peak
98,932 requested bytes: oversized reserves have a measurable cost. The
synthetic workload does not select the playable game's budgets. Compare modes
within a capacity/load; different capacities can reject different work.
These local results are not portable performance guarantees. CSVs and machine
metadata remain build artifacts rather than tracked source.

## Tests and comparison evidence

```bash
cmake --build build --target neon-rules-tests neon-storage-tests neon-arena-tests neon-ui-tests neon-allocation-probe-tests neon-siege-memory-benchmark neon-siege-memory-profile -j 4
ctest --test-dir build --output-on-failure -R '^neon-'
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./build/neon-siege --smoke
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./build/neon-siege --screenshot build/neon-scene.png
python3 individual-games/harsha/neon-siege/compare_games.py > build/neon-source-comparison.json
```

Tests cover rule boundaries, full quotas, reused state, stale generations,
same-ID contacts in consecutive collision passes, duplicate overlaps, damage
protection, forty collision-driven boss hits, defeat precedence, formation
reversal/descent, expiry, frame partitioning and cross-backend parity through
wave transitions. UI tests drive real engine input/chords and software rendering.
The scripted smoke must clear both formations and terminate at the boss; it
can lose there. It does not establish human playability or difficulty balance.

The source comparison reuses Frostbrew's comparison helpers and reports all
three game pairs. It compares sorted raw client-source lines, including
comments/braces/blanks, while excluding engine code, tests, benchmarks, assets
and servers. It measures source similarity, not a percentage of engine reuse.

Focused ASan+UBSan runs passed for rules, storage, arena, UI, full-match smoke,
allocation-hook self-tests and the timing workload. LeakSanitizer was not run;
SDL remained uninstrumented. Human playtesting, networking and additional
content are outside this version. Unexpected allocation failures are fatal to
the current run; callers must not catch them and continue a partially built scene.

Gameplay inspiration: Space Invaders; ships and game code are original.
Memory-study background: Robert Nystrom's
[Object Pool chapter](https://gameprogrammingpatterns.com/object-pool.html).
