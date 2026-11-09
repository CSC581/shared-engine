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

That completes the mission, and the player who picked up the most cargo is
named the winner.

## Gameplay

Two shuttles move between decks: one slides sideways and the other is a lift.
Drones patrol the station, and one of them flies in a circle. Touching a drone
costs a life and leaves you down (see reviving below). Everyone starts with 3
lives. Falling into the hull breach, the red gap in the floor, sends you back
to your spawn point without costing a life.

The camera follows your astronaut as the station scrolls sideways. Every
player sees the others move, and the cargo, gate and relays are shared over
the network.

### Added for Milestone 4

Purple gravity panels flip your gravity. After stepping on one you fall up,
walk on the ceiling and the undersides of decks, and jump downward. The panels
on the ceiling turn gravity back to normal. The crate above the breach is
easiest to reach upside down.

When a drone takes you down, your astronaut floats in place with an SOS
beacon. After 10 s the beacon sends you back to your spawn point. If that hit
used your last life it takes 20 s instead, and you come back with all 3 lives.
A teammate can bring you back sooner by standing next to you and holding
`S` + `E` for about a second, and you then reappear next to them. A revive
only counts if the rescuer is actually close to you. Everyone sees messages
such as "NOVA IS DOWN" and "SOL REVIVED NOVA".

Every 35 to 50 s a station alarm goes off for the whole crew, after a 2 s
warning. A gravity failure flips everyone's gravity for 6 s. A blackout
darkens the station for 8 s, except around astronauts and relay pads. Alarms
come from one player's game, the alarm director, which is whoever is online
with the lowest player id. If that player leaves, the next one takes over.

The game always keeps a recording of the last 10 seconds. When the relays come
online, it plays that recording back in slow motion as a mission debrief. If
you are recording your own clip with `F9` at that moment, the debrief plays
your clip instead, which also ends on the win. If the relays come online while
you are already watching a replay, nothing of that moment was recorded, so
there is no debrief. You can also record and play back your own clips (see
Controls).

## Milestone 3 requirements

| Requirement | In the game |
| --- | --- |
| Player characters that move and jump | Each client controls an astronaut that can walk, jump and double jump |
| At least 3 static platforms | 7 decks in three colours: a navy floor, violet decks in the west and teal decks in the east wing |
| Moving platforms with 2+ movement patterns | A sideways shuttle, a vertical lift and a drone that flies in a circle |
| More than one hidden spawn point | 8 invisible spawn points |
| A hidden death zone that sends you to a spawn point | An invisible zone inside the hull breach, plus catch-all zones below the station and above the roof |
| Side-scrolling | The camera follows the player across a station wider than the screen |

Everything in the station is a `GameObject` built from engine components. The
design decisions are written up in
`docs/individual-games/vanaja/starfall-salvage-design.md`.

## Milestone 4 requirements

| Requirement | In the game |
| --- | --- |
| Collision, death, spawn and input events drive gameplay | Every control is an `InputEvent`, and reviving uses an `S` + `E` chord. Drones, death zones, crates and gravity panels work through `CollisionEvent`. A drone hit raises a `DeathEvent` that takes you down, and the breach raises one that sends you back. Every respawn, beacon recall and revive is a `SpawnEvent` |
| A networked interaction that relies on events | Deaths, revives and alarms reach the other players as `DeathEvent`, `ReviveEvent` and `StationAlarmEvent`, through the engine's `NetworkEventBridge` and the server's event relay |
| Different from teammates' games | Co-op reviving, walking on the ceiling, crew-wide alarms, and a replay when the mission succeeds rather than when you die |
| Replay (581) | The debrief starts on its own when the relays come online. Clips can also be recorded, played, sped up and paused with `F9` to `F12`. Both use the engine's replay events |

The gameplay events are handled in `StationEvents`
(`StarfallSalvageEvents.hpp` and `StarfallSalvageLevel.cpp`). It does not use
SDL, so the tests can run it without a window.

Networked events only work in client-server mode. In peer-to-peer mode there
are no alarms, deaths and revives are not shared, and a downed player has to
wait for the beacon.

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target starfall-salvage starfall-salvage-server -j 4
```

## Run (client-server)

Start the server and three players, each in its own terminal:

```bash
./build/starfall-salvage-server
./build/starfall-salvage --name Nova
./build/starfall-salvage --name Sol
./build/starfall-salvage --name Mira
```

To turn the alarms off while testing, add `--no-alarms` to the first player to
join. That player normally has the lowest id, which makes it the alarm
director.

## Run (peer-to-peer)

The server still runs the shuttles and drones:

```bash
./build/starfall-salvage-server
./build/starfall-salvage --name Nova --mode peer-to-peer --peer-id 1 --port 7100
./build/starfall-salvage --name Sol  --mode peer-to-peer --peer-id 2 --port 7200 --bootstrap tcp://127.0.0.1:7100
./build/starfall-salvage --name Mira --mode peer-to-peer --peer-id 3 --port 7300 --bootstrap tcp://127.0.0.1:7100
```

To play on separate laptops, start the server with `--advertise SERVER_IP` and
add `--server tcp://SERVER_IP:5555` to each player. In peer-to-peer mode, each
peer also adds `--advertise ITS_OWN_IP`, and `--bootstrap` uses the first
peer's IP.

## Controls

| Key | Action |
| --- | --- |
| `A` / `D` or Left / Right | Move |
| `Space`, `W` or Up | Jump, and press again in the air to double jump. Jumps go downward while flipped |
| Hold `E` | Use the relay pad or gate switch you are standing on |
| Hold `S` + `E` (or Down + `E`) | Revive a downed teammate next to you |
| `F9` | Start or stop recording a clip (keeps the last 60 s) |
| `F10` | Play the last 10 s, or your clip. Press again to stop |
| `F11` | Replay speed: 0.5x, 1x or 2x |
| `F12` | Pause the replay |
| `P` | Pause the game |
| `1` / `2` / `3` | Game speed 0.5x / 1x / 2x |
| `F1` | Window scaling |
| `F2` | Diagnostics |
| `Esc` | Quit |

All of these keys go through the engine's `InputEvent`; the game never polls
the keyboard.

Only the window in focus gets keyboard input, so one person can test almost
everything on one laptop by switching windows. The exceptions are the two team
actions, crossing the gate and finishing on the relays, which need players
holding keys at the same time.

## Tests

```bash
cmake --build build --target starfall-salvage-tests
ctest --test-dir build -R starfall --output-on-failure
```
