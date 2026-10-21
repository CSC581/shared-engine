# Starfall Salvage

A three-player co-op platformer on a space station, built on the team engine.

## Mission

A damaged station needs its relay array back online. The team must:

1. **Collect the 4 cargo crates.** Touch a crate to pick it up. One crate is in
   the east wing, behind a locked gate.
2. **Open the gate together.** One player stands on the orange switch beside
   the gate and holds `E` while a teammate walks through. A second switch on
   the far side lets them cross back.
3. **Bring the relays online.** With all 4 crates collected, each player stands
   on a different relay pad (left, middle and east wing) and all three hold `E`
   at the same time.

The mission is then complete, and the player who collected the most cargo is
named the winner.

## How it works

- Shuttles carry you between decks: one slides sideways, and a lift goes up
  and down.
- Drones patrol the station (one flies in a circle). Touching one costs a life
  and sends you back to your spawn point. You have 3 lives; press `R` to
  respawn after losing them all.
- Falling into the hull breach (the red gap in the floor) sends you back to
  your spawn point without losing a life.
- The camera follows you as the station scrolls sideways.
- Every player sees everyone else move, and progress (cargo, gate, relays) is
  shared across the network.

## Milestone 3 requirements

| Requirement | What the game has |
| --- | --- |
| Player characters (move left/right, jump) | Each client controls an astronaut: walk, jump, double jump |
| 3+ static platforms (unique position, size, colour) | 7 decks in 3 colours: navy floor, violet west decks, teal east wing |
| Moving platforms (2+ movement patterns) | A sideways shuttle, a vertical lift, and a drone flying in a circle |
| Hidden spawn points (more than one) | 8 invisible spawn objects |
| Hidden death zone that teleports to a spawn point | An invisible zone inside the hull breach (the red gap in the floor), plus one below the station |
| Side-scrolling | A camera follows the player across a station wider than the screen |

Everything in the station is a `GameObject` made of engine components. Design
decisions are in `docs/individual-games/vanaja/starfall-salvage-design.md`.

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target starfall-salvage starfall-salvage-server -j 4
```

## Run (client-server)

Use four terminals:

```bash
./build/starfall-salvage-server
./build/starfall-salvage --name Nova
./build/starfall-salvage --name Sol
./build/starfall-salvage --name Mira
```

## Run (peer-to-peer)

The server still supplies the shuttles and drones:

```bash
./build/starfall-salvage-server
./build/starfall-salvage --name Nova --mode peer-to-peer --peer-id 1 --port 7100
./build/starfall-salvage --name Sol  --mode peer-to-peer --peer-id 2 --port 7200 --bootstrap tcp://127.0.0.1:7100
./build/starfall-salvage --name Mira --mode peer-to-peer --peer-id 3 --port 7300 --bootstrap tcp://127.0.0.1:7100
```

To play on separate laptops, start the server with `--advertise SERVER_IP` and
add `--server tcp://SERVER_IP:5555` to each player. In peer-to-peer, each peer
also adds `--advertise ITS_OWN_IP`, and `--bootstrap` uses the first peer's IP.

## Controls

| Key | Action |
| --- | --- |
| `A` / `D` or arrows | Move |
| `Space` / `W` | Jump (press again in the air to double jump) |
| Hold `E` | Use the relay or gate switch you are standing on |
| `R` | Respawn after losing all 3 lives |
| `P` | Pause |
| `1` / `2` / `3` | 0.5x / 1x / 2x speed |
| `F1` / `F2` | Window scaling / diagnostics |
| `Esc` | Quit |

Only the focused window gets keyboard input, so one person on one laptop can
test everything except the two team actions (crossing the gate and finishing
on the relays).

## Tests

```bash
cmake --build build --target starfall-salvage-tests
ctest --test-dir build -R starfall --output-on-failure
```
