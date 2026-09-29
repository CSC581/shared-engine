# Starfall Salvage

This is the cooperative Milestone 2 iteration of Starfall Salvage. The same
astronaut, code-drawn station, four cargo crates, repair-drone hazards, lives,
and double-jump return from Milestone 1. Players now recover cargo together,
cross two server-controlled shuttles, cooperate to open the east-wing bulkhead,
then stand on three different relay pads and hold `E` at the same time to
complete the mission. Cargo pickups are shared through the players' compact
network data, so collecting the same crate twice does not add progress. The
same SDL game runs in client-server or hybrid peer-to-peer mode. In hybrid
mode, peers exchange player state, cargo progress, and switch claims directly;
the headless server supplies only shuttle and drone poses. There is no fully
decentralized extra-credit mode.

## Build

From the repository root, with the vendored submodules present:

```bash
cmake -S . -B build
cmake --build build --target starfall-salvage starfall-salvage-server starfall-salvage-tests -j 4
ctest --test-dir build -R '^starfall-salvage-tests$' --output-on-failure
```

## Run three client-server players

Rebuild the game on every computer before playing together; older client builds
do not understand the personal cargo field used to choose a winner.
Use four terminals, one process per terminal:

```bash
./build/starfall-salvage-server --rates
./build/starfall-salvage --name Nova
./build/starfall-salvage --name Sol
./build/starfall-salvage --name Mira
```

Each client can be launched before or after the server. Collect all four orange
cargo crates by touching them. The red patrol drones cover long, marked routes,
so watch their timing and jump over them. To reach the last crate, one player
must stand on the switch west of the bulkhead and hold `E` while another crosses.
The second switch in the east wing opens the bulkhead for the first player to
follow. If you lose all three lives, press `R` to respawn. Once the cargo
counter reaches 4/4, put one astronaut on each relay and hold `E` together.
The mission stays complete only while all three relays remain occupied. When
that happens, the astronaut with the most personal cargo pickups is named the
winner; ties go to the lower player ID so every client shows the same result.
If two players reach a crate before either receives the other's update, both
get personal credit but the team cargo counter increases only once. Your
personal pickup count appears in the lower-right HUD. The camera follows your
astronaut horizontally through the wider station; the HUD stays fixed. The last
cargo crate and relay are in the east wing. Camera scrolling changes only what
you see; it does not change world-space positions exchanged over the network.

Set one client to `1` (0.5x), one to `2` (1x), and one to `3` (2x) to compare
their measured loop rates in the client HUD. With `--rates`, the server also
prints accepted `POSITION` updates per second for each player. The publish-call
counter in `F2` diagnostics is not a wire-message count: the networking layer
can skip a send while awaiting a reply. For a repeatable measurement without
key presses, add `--scale 0.5`, `--scale 1`, or `--scale 2` to the three client
commands.

## Run three hybrid peers

Start the same server, then use three more terminals:

```bash
./build/starfall-salvage --name Nova --mode peer-to-peer --peer-id 1 --port 7100
./build/starfall-salvage --name Sol --mode peer-to-peer --peer-id 2 --port 7200 --bootstrap tcp://127.0.0.1:7100
./build/starfall-salvage --name Mira --mode peer-to-peer --peer-id 3 --port 7300 --bootstrap tcp://127.0.0.1:7100
```

Peers exchange player positions, cargo progress, and control-pad claims directly.
They make read-only `GET_WORLD` requests to the server for shuttle and drone
positions; they do not `JOIN` it or consume its player slots. Begin with peer
1, or launch in a different order and allow discovery time. Do not mix
client-server and peer players in one station session; they use separate
player rosters.

For different computers on a LAN, run the server with a reachable
`--advertise` host and give each client `--server tcp://SERVER_IP:5555`. Peers
also need unique IDs and port pairs, `--advertise THEIR_LAN_IP`, and a
`--bootstrap tcp://PEER_IP:PORT` address. Open the server handshake and its
dynamic worker ports, and each peer's two consecutive ports in the firewall.
The localhost commands above are the simplest grading demonstration.

## Controls and evidence

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

The pause is *local*: that astronaut and its game-time effects stop, while
other players and server-authored shuttles and drones continue. The game keeps
polling networking, so it can recover without unpausing. A missing world
authority freezes the last known moving objects and shows a warning; a
restarted server resets their paths rather than restoring a saved world.
Connected peers gossip the cargo bitmask, so a late joiner learns progress from
an existing player. If every player leaves, cargo progress is not persisted.

Useful screenshots for the individual reflection are (1) shared cargo progress
and a drone, (2) all three relays completing the salvage mission, (3) different
client loop rates with server-accepted counts, and (4) hybrid peers using the
same server-owned shuttle.
