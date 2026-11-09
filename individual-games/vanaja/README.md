# Starfall Salvage

A three-player co-op platformer on a space station, built on the team engine.

## Mission

A damaged station needs its relay array back online. The team must:

1. **Collect the 4 cargo crates.** Touch a crate to pick it up. One crate is in
   the east wing, behind a locked gate, and one hangs under the roof above the
   hull breach.
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
- Drones patrol the station (one flies in a circle). Touching one leaves you
  **down** for up to 10 s (see co-op revive below).
- Falling into the hull breach (the red gap in the floor) sends you straight
  back to your spawn point.
- The camera follows you as the station scrolls sideways.
- Every player sees everyone else move, and progress (cargo, gate, relays) is
  shared across the network.

### Milestone 4 features

- **Gravity flip panels:** step on a glowing purple panel and your gravity
  flips. You fall up, walk on the ceiling and the undersides of decks, and jump
  downward. Panels on the ceiling flip you back. The crate over the hull breach
  is easiest to reach upside down.
- **Co-op revive:** a downed astronaut floats in place with an SOS beacon,
  and after 10 s the beacon brings them back at their spawn point. A teammate
  standing next to them and holding `S` + `E` for about a second brings them
  back sooner, at the rescuer's side. A revive only counts if the rescuer is
  really next to you. Everyone sees "NOVA IS DOWN" and "SOL REVIVED NOVA".
- **Station alarms:** every 35 to 50 s the crew's alarm director (whoever is
  online with the lowest player id; the next one takes over if they leave)
  sets off an alarm for the
  whole crew: a **gravity failure** (everyone's gravity flips for 6 s) or a
  **blackout** (only the space around astronauts and relays stays lit for 8 s).
  A 2 s warning comes first.
- **Mission debrief:** the game always keeps the last 10 seconds. When the
  relays come online, it plays them back in slow motion (if you are recording
  an F9 clip at that moment, the debrief plays that clip instead, which also
  ends on the win). If the relays come online while you are watching a
  replay, there is no footage of that moment and the debrief is skipped. You
  can also record and replay your own clips (see controls).

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

## Milestone 4 requirements

| Requirement | What the game has |
| --- | --- |
| Collision, death, spawn and input events drive gameplay | `InputEvent` for every control, plus an `S + E` input chord for reviving. `CollisionEvent` for drones, death zones, crates and gravity panels. `DeathEvent` for drone hits (go down) and the breach (respawn). `SpawnEvent` for every respawn, beacon recall and revive |
| A networked interaction that relies on events | Deaths, revives and station alarms travel as `DeathEvent`, `ReviveEvent` and `StationAlarmEvent` through the engine's `NetworkEventBridge` and the server's event relay |
| Unique from teammates | Co-op revive, gravity flipping to walk on ceilings, crew-wide alarms, and a replay on mission success rather than on death |
| Replay (581) | An in-game trigger (the relays coming online plays a slow-motion debrief) and manual record, play, speed and pause, all through the engine's replay events |

Gameplay events are handled by `StationEvents` (declared in
`StarfallSalvageEvents.hpp`, implemented in `StarfallSalvageLevel.cpp`), which has no SDL
and is covered by the tests. Networked events are client-server only, so in
peer-to-peer mode there are no alarms, deaths and revives are not shared, and a
downed player waits for the beacon.

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target starfall-salvage starfall-salvage-server -j 4
```

## Run (client-server)

Use four terminals (add `--no-alarms` to the player with the lowest id, normally
the first to join, to turn the station alarms off while testing):

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
| `Space` / `W` | Jump (press again in the air to double jump); jumps downward while flipped |
| Hold `E` | Use the relay or gate switch you are standing on |
| Hold `S` + `E` (or `Down` + `E`) | Revive a downed teammate you are standing next to |
| `F9` | Start or stop recording a replay clip (keeps its last 60 s) |
| `F10` | Play the replay (the last 10 s, or your clip); press again to stop |
| `F11` / `F12` | Replay speed 0.5x / 1x / 2x, and pause |
| `P` | Pause |
| `1` / `2` / `3` | 0.5x / 1x / 2x speed |
| `F1` / `F2` | Window scaling / diagnostics |
| `Esc` | Quit |

Every key above is handled through the engine's `InputEvent`; the game never
polls the keyboard.

Only the focused window gets keyboard input, so one person on one laptop can
test everything except the two team actions (crossing the gate and finishing
on the relays).

## Tests

```bash
cmake --build build --target starfall-salvage-tests
ctest --test-dir build -R starfall --output-on-failure
```
