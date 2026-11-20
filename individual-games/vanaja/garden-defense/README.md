# Garden Defense

Garden Defense is a single-player Space Invaders game on our team engine.
Rows of pests march toward a vegetable garden, and you spray them with a
watering can before they reach the fence.

## How to play

- The pests march sideways, step down at each edge and speed up as their
  numbers drop. If one reaches the fence, the game is over.
- Spray them with water drops. At most three drops can be in the air at once.
  Beetles (top row) are worth 40 points, caterpillars 30, and aphids 20 and 10.
- The lowest pest in each column drops seeds. A seed that hits the can costs
  one of your 3 lives. The can comes back after a second and blinks while it
  is briefly protected.
- Hold Left and Right together to open an umbrella that blocks seeds for 1.5
  seconds. It then needs 6 seconds to recharge.
- A ladybug crosses the top of the screen every 20 to 30 seconds. Spraying
  it is worth 150 points.
- Clear the first wave and a second one arrives, starting lower and marching
  faster. Clear both to save the garden.

## Build and run

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target garden-defense garden-defense-tests -j 4
./build/garden-defense
./build/garden-defense-tests
```

`--seed N` repeats the same seed drops and ladybug timings.

## Controls

| Key | Action |
| --- | --- |
| `Left` / `Right` or `A` / `D` | Move the can |
| `Space` | Spray a water drop |
| `Left` + `Right` (or `A` + `D`) | Open the umbrella |
| `P` | Pause or resume |
| `F3` | Show or hide the pool counter |
| `R` | Play again after a win or a loss |
| `F1` | Switch between proportional and constant window scaling |
| `Esc` | Quit |

## Milestone 5 requirements

| Requirement | Where it is met |
| --- | --- |
| Section 1: memory pool | Every object in a match comes from one pooled `World` of 160 slots (`World world_{objectCapacity}` in `GardenDefenseMatch.hpp`). Gameplay objects always fit; splats and petals use `tryCreate()` and are skipped near the limit. `F3` shows the pool's use and the skipped count |
| Section 2: input chord | `Left` + `Right` (and `A` + `D`) are registered with `engine.registerChord`. Alone, each key moves the can; together they would cancel out, so the chord opens the umbrella instead |
| Section 3: a second game that is not a platformer | One level of Space Invaders with a second wave: rows of pests that move across and down, and a can that moves and fires |

## Engine features used

- `World`, `GameObject` and components: `Transform`, `Motion`, `Gravity`
  (splats and petals), `PathMover` (the ladybug), `Collider` and `Renderable`.
- Collision, death and spawn events: the World raises `CollisionEvent`s, which
  become `DeathEvent`s and `SpawnEvent`s. Respawns and the next wave are
  `SpawnEvent`s raised with a delay on game time, so they wait while the game
  is paused.
- Input events and chords, the game timeline for pause, and `renderWorld()`.

The gameplay is in `GardenDefense::Match`, which has no SDL code, so the tests
run without a window. Design notes are in
[`docs/individual-games/vanaja/garden-defense-design.md`](../../../docs/individual-games/vanaja/garden-defense-design.md).
