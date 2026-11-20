# Pirate Cannons

Pirate Cannons is a Gorillas-style game on our team engine. Your ship and a
computer-controlled ship take turns firing cannonballs at each other across a
sea, over a rocky island.

## How to play

- On your turn, set the cannon's angle and power, then fire. The wind changes
  every turn and pushes the cannonball sideways. The arrows at the top right
  show its direction and strength.
- The island and a flock of gulls are in the way. A gull that is hit flies
  off until the next round.
- Three hits sink a ship. Sink the other ship to win the round; the first to
  win 2 rounds wins the match. Rounds take turns on who fires first.
- The computer aims at your ship but misses by a margin that shrinks after
  each of its misses, so it gets closer the longer a round lasts.
- Twice per match you can fire a chain shot, a cannonball that splits into
  three as it starts to come down.

## Build and run

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target pirate-cannons pirate-cannons-tests -j 4
./build/pirate-cannons
./build/pirate-cannons-tests
```

`--seed N` repeats the same wind and computer aim.

## Controls

| Key | Action |
| --- | --- |
| `Up` / `Down` | Raise or lower the cannon |
| `Left` / `Right` | Less or more power |
| `Space` | Fire |
| `Left` + `Right` + `Space`, pressed together | Fire a chain shot (twice per match) |
| `P` | Pause or resume |
| `F3` | Show or hide the pool counter |
| `R` | Play again after the match |
| `F1` | Switch between proportional and constant window scaling |
| `Esc` | Quit |

## Milestone 5 requirements

| Requirement | Where it is met |
| --- | --- |
| Section 1: memory pool | Every object in a match comes from one pooled `World` of 96 slots (`World world_{objectCapacity}` in `PirateCannonsMatch.hpp`). Ships, the island, gulls and cannonballs always fit; splinters, splashes and feathers use `tryCreate()` and are skipped near the limit. `F3` shows the pool's use and the skipped count |
| Section 2: input chord | `Left` + `Right` + `Space` is registered with `engine.registerChord`. Alone, `Left` and `Right` change the power and `Space` fires; pressed together (all within 0.2 s) they fire the chain shot |
| Section 4: another game that is not a platformer | Gorillas: two ships in turn-based artillery, with wind, obstacles and a computer opponent |

## Engine features used

- `World`, `GameObject` and components: `Transform`, `Motion` and `Gravity`
  for the cannonballs, a `Behavior` that applies the wind, `PathMover` for the
  gulls (two straight paths and two circles), `Collider` and `Renderable`.
- Collision, death and spawn events: a cannonball's `CollisionEvent` damages a
  ship, a sunk ship raises a `DeathEvent`, and the next round is a
  `SpawnEvent` raised with a delay on game time, so it waits while the game is
  paused.
- Input events and chords, the game timeline for pause, and `renderWorld()`.

The lifetime component for cosmetic objects, the pool reserve check, frame
pacing, the drawing helpers and the `F3` pool counter come from `../common/`,
which both Milestone 5 games share.

The gameplay and the computer's aim are in `PirateCannons::Match` and
`PirateCannonsRules`, which have no SDL code, so the tests run without a
window. Design notes are in
[`docs/individual-games/vanaja/pirate-cannons-design.md`](../../../docs/individual-games/vanaja/pirate-cannons-design.md).
