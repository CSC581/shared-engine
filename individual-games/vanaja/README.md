# Starfall Salvage

Starfall Salvage is a three-player space-station game. Collect the four cargo
crates, help each other open the bulkhead, then stand on the three relay pads
and hold `E` together to finish. Avoid the drones and use the moving shuttles
to reach other parts of the station. The player who collected the most cargo
is named the winner. If two players claim the same crate before their updates
arrive, only the player with the lower ID gets credit once claims are shared.

The same game can run in client-server or hybrid peer-to-peer mode. Both modes
use the game server for the moving shuttles and drones.

## How to play

1. Move with `A`/`D` or the arrow keys. Press `Space` twice to double-jump, and
   use the moving shuttles to reach higher decks: one slides sideways from the
   west floor to the middle deck, and a lift rises from there to the deck by the
   gate. Avoid the patrolling drones, including the one circling above the
   middle deck, and the hull breach in the floor beneath it, marked with hazard
   stripes and a red glow: falling in sends you back to your spawn point.
2. Touch the orange cargo crates. The counter at the top shows the team's
   progress. One crate is on the raised deck in the east wing, beyond the
   tall red gate.
3. To open the gate, one player stands on the orange switch just to its left
   and holds `E` while another player crosses. There is a second switch on the
   right that can help a teammate cross back.
4. Once the counter reads `CARGO 4/4`, put one different player on each of the
   three relay pads: left, middle, and east wing. All three players must stand
   on their pads and hold `E` at the same time to finish. The game then names
   the player who earned the most cargo credit as the winner.

You can open three clients on one laptop to check that networking works, but
one keyboard cannot reliably control all three windows at once for the gate
and final relay step. Playing together on separate laptops is easier.

## Build

From the repository root:

```bash
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --target starfall-salvage starfall-salvage-server -j 4
```

## Client-server mode

Open four terminals. Start the server in the first terminal, then run one
client in each of the other three:

```bash
./build/starfall-salvage-server
```

```bash
# Run one of these commands in each of the other three terminals
./build/starfall-salvage --name Nova
./build/starfall-salvage --name Sol
./build/starfall-salvage --name Mira
```

In this mode, clients send their positions to the server, which shares player
and moving-world state with everyone.

## Hybrid peer-to-peer mode

Use four terminals again. Start the game server in the first terminal. Run
the three peer commands in separate terminals, starting with Nova:

```bash
./build/starfall-salvage-server
```

```bash
./build/starfall-salvage --name Nova --mode peer-to-peer --peer-id 1 --port 7100
```

```bash
./build/starfall-salvage --name Sol --mode peer-to-peer --peer-id 2 --port 7200 --bootstrap tcp://127.0.0.1:7100
```

```bash
./build/starfall-salvage --name Mira --mode peer-to-peer --peer-id 3 --port 7300 --bootstrap tcp://127.0.0.1:7100
```

Here, players share their positions directly with each other. The server
supplies only the moving shuttles and drones. Each peer needs a different ID
and port. Do not mix client-server clients with peer-to-peer clients in the
same game.

These examples run on one computer. To play across computers on the same
network, start the server with `--advertise SERVER_IP`, add
`--server tcp://SERVER_IP:5555` to each client command, and give each peer
`--advertise ITS_OWN_IP`. Replace each `--bootstrap` address with the first
peer's reachable IP and port.

## Controls

| Key | Action |
| --- | --- |
| `A` / `D` or arrows | Move |
| `Space` or `W` | Jump, then jump once more in midair |
| Hold `E` | Activate the relay or bulkhead switch beneath your feet |
| `R` | Respawn after all three lives are lost |
| `P` | Pause or resume local game time |
| `1` / `2` / `3` | 0.5x / 1x / 2x game time and loop rate |
| `F1` | Toggle proportional/constant window scaling |
| `F2` | Toggle clock, worker, and publish diagnostics |
| `Esc` | Quit |

## Engine features

Everything in the station is a `GameObject` in one `World`, built from engine
components by factory functions in `StarfallSalvageLevel.cpp`:

| Object | Components |
| --- | --- |
| Astronaut (you) | `Transform`, `Motion`, `Gravity`, solid `Collider`, and the game's own `Astronaut` component for input, double jump, landing, shuttle riding and hazards |
| Decks | `Transform`, solid `Collider`, `Renderable`, and a `DeckStyle` that picks one of three colour schemes (hull floor, west decks, east wing) |
| Shuttles and drones | Made by `NetworkWorldSync` from the server's platforms: shuttles are solid and drawn, drones are hazard triggers |
| Other astronauts | Made by `NetworkWorldSync`; removed when that player leaves |
| Spawn points | `Transform` only, so they are never drawn |
| Death zones | A hidden trigger `Collider` in the hull breach, plus a catch-all below the station |
| Cargo, relay pads, gate switches | `Transform` and `Renderable`; the game changes their colour and visibility |
| Bulkhead | A solid `Collider` whose layers are cleared while the gate is open |

The engine's collision pass pushes the astronaut out of decks, shuttles and the
closed bulkhead, and the `Astronaut` component's collider callbacks work out
what happened: standing on something, touching a drone, or falling into a
death zone. The engine does not carry riders, so the game moves the astronaut
by however far its shuttle moved since the last frame.

`FrameWorkers` runs two update tasks: the astronaut's components (through
`World::updateComponents` for the `player` tag) on one worker, and remote
relay/cargo checks and visual effects on the other. Both receive the same
game-time delta. Once both finish, the main thread runs the collision pass,
then sends player state and draws the frame. Input, networking calls,
collisions and rendering stay on the main thread. The engine joins the workers
when the game closes.

`Multiplayer::Session` handles both networking modes, and `NetworkServerHost`
runs the game server. Game code does not create ZeroMQ sockets. The server
controls moving shuttles and drones; clients use the received positions rather
than calculating their paths again.

## Time and message rates

The game runs at a 50-loop-per-second baseline: frames follow a fixed 20 ms
schedule, so small sleep delays do not add up and slow the loop. That keeps
each frame's fall short enough for the engine's collision pass to land the
astronaut instead of pushing it off a deck edge.
Keys `1`, `2`, and `3` change both local game time and the actual loop rate.
The session also uses the engine's send pacing, with
`sendIntervalGameTics = kGameTicsPerSecond / 50`. The game gives it a separate
timeline at the same scale as game time, but does not pause that timeline.

| Setting | Target loops per real second | Target position updates per real second |
| --- | --- | --- |
| 0.5x | 25 | 25 |
| 1x | 50 | 50 |
| 2x | 100 | 100 |

These are targets, not guaranteed network rates. A busy frame or pending
server reply can lower the actual count. `F2` shows loop and worker counters;
its publish counter counts game API calls, not delivered messages.

Pausing stops local movement and game-timed effects, but polling and recovery
continue. Network publishing keeps its current speed during pause, so the
server can send fresh player and platform positions instead of leaving them
frozen between sparse heartbeats. This sends more messages while paused than
the engine's minimum heartbeat. Your astronaut stays still even on a moving
shuttle. If it moves away, the astronaut may fall when you resume.

To see accepted player messages in client-server mode, start the server with:

```bash
./build/starfall-salvage-server --rates
```

Use `--scale 0.5`, `--scale 1`, and `--scale 2` on the three client commands,
or change speeds with the keys. Compare several seconds of server output.
In hybrid mode, player updates go directly between peers, so the server's
player-message counter does not measure them.

## Automated checks

```bash
cmake --build build --target starfall-salvage-tests frame-workers-tests multiplayer-tests -j 4
ctest --test-dir build -R "starfall-salvage|frame-workers|multiplayer" --output-on-failure
```

These cover the game's mission rules and timing settings, the station's
objects (distinct platforms, hidden spawn points and death zones, three
movement patterns), the astronaut's physics through the engine's real
collision pass (standing, jumping, edge landings, the breach, drones and
shuttle riding), the engine's worker barrier and shutdown, and multiplayer
pacing. They do not replace playing a full round with three clients.
