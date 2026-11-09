# Starfall Salvage

Starfall Salvage is a co-op platformer for three players, set on a damaged
space station. It runs on our team engine and uses the engine's event system
and networking.

## The mission

The station's relay array is offline, and the crew has to bring it back:

1. Collect the four cargo crates by touching them. One is in the east wing
   behind a locked gate, and one hangs from the roof above the hull breach.
2. Open the gate. One player holds `E` on the orange switch next to the gate
   while a teammate walks through. A second switch on the far side lets them
   get back.
3. Once all four crates are in, each player stands on a different relay pad
   (left, middle and east wing), and all three hold `E` at the same time.

The player who picked up the most cargo is named the winner.

## Gameplay

Two shuttles move between decks, and drones patrol the station. Touching a
drone costs a life and leaves you down. Everyone starts with 3 lives. Falling
into the hull breach, the red gap in the floor, sends you back to your spawn
point without costing a life.

Purple gravity panels flip your gravity. After stepping on one you fall up,
walk on the ceiling and jump downward. The panels on the ceiling turn gravity
back to normal.

When you are down, an SOS beacon sends you back to your spawn point after
10 s. If that hit used your last life it takes 20 s, and you come back with
all 3 lives. A teammate can bring you back sooner by standing next to you and
holding `S` + `E` for about a second.

Every 35 to 50 s a station alarm goes off for the whole crew, after a 2 s
warning. A gravity failure flips everyone's gravity for 6 s, and a blackout
darkens the station for 8 s. Alarms come from the player with the lowest id.

The game always keeps the last 10 seconds. When the relays come online, it
plays them back in slow motion as a mission debrief. You can also record and
play your own clips (see Controls).

## Milestone 4 requirements

| Requirement | In the game |
| --- | --- |
| Collision, death, spawn and input events drive gameplay | Every control is an `InputEvent`, and reviving uses an `S` + `E` chord. Drones, death zones, crates and gravity panels work through `CollisionEvent`. A drone hit raises a `DeathEvent` that takes you down, and the breach raises one that sends you back. Every respawn, beacon recall and revive is a `SpawnEvent` |
| A networked interaction that relies on events | Deaths, revives and alarms reach the other players as `DeathEvent`, `ReviveEvent` and `StationAlarmEvent`, through the engine's `NetworkEventBridge` and the server's event relay |
| Different from teammates' games | Co-op reviving, walking on the ceiling, crew-wide alarms, and a replay when the mission succeeds rather than when you die |
| Replay (581) | The debrief starts on its own when the relays come online. Clips can also be recorded, played, sped up and paused with `F9` to `F12`. Both use the engine's replay events |

Design decisions are in `docs/individual-games/vanaja/starfall-salvage-design.md`.

## Build and run

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target starfall-salvage starfall-salvage-server -j 4

./build/starfall-salvage-server
./build/starfall-salvage --name Nova
./build/starfall-salvage --name Sol
./build/starfall-salvage --name Mira
```

Run the server and each player in its own terminal. Add `--no-alarms` to the
first player to turn the alarms off while testing. To play on separate
laptops, start the server with `--advertise SERVER_IP` and add
`--server tcp://SERVER_IP:5555` to each player.

Only the window in focus gets keyboard input, so you can test most things on
one laptop by switching windows.

## Peer-to-peer

Players can also connect directly to each other. The server still runs the
shuttles and drones:

```bash
./build/starfall-salvage-server
./build/starfall-salvage --name Nova --mode peer-to-peer --peer-id 1 --port 7100
./build/starfall-salvage --name Sol  --mode peer-to-peer --peer-id 2 --port 7200 --bootstrap tcp://127.0.0.1:7100
./build/starfall-salvage --name Mira --mode peer-to-peer --peer-id 3 --port 7300 --bootstrap tcp://127.0.0.1:7100
```

Networked events only work in client-server mode, so in peer-to-peer there are
no alarms, deaths and revives are not shared, and a downed player has to wait
for the beacon. On separate laptops, each peer also adds
`--advertise ITS_OWN_IP`, and `--bootstrap` uses the first peer's IP.

## Controls

| Key | Action |
| --- | --- |
| `A` / `D` or Left / Right | Move |
| `Space`, `W` or Up | Jump, and press again in the air to double jump |
| Hold `E` | Use the relay pad or gate switch you are standing on |
| Hold `S` + `E` (or Down + `E`) | Revive a downed teammate next to you |
| `F9` | Start or stop recording a clip |
| `F10` | Play the last 10 s, or your clip. Press again to stop |
| `F11` | Replay speed: 0.5x, 1x or 2x |
| `F12` | Pause the replay |
| `P` | Pause the game |
| `1` / `2` / `3` | Game speed 0.5x / 1x / 2x |
| `F1` | Window scaling |
| `F2` | Diagnostics |
| `Esc` | Quit |

## Tests

```bash
cmake --build build --target starfall-salvage-tests
ctest --test-dir build -R starfall --output-on-failure
```
