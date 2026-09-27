# Signal Bloom

Signal Bloom is Vanaja's cooperative Project 2 game. Three astronauts cross a
damaged station using two server-controlled shuttles. Each player stands at a
different relay and holds `E`; all three links light the station at once. The
same SDL game runs in client-server or hybrid peer-to-peer mode. The headless
server owns only the shared moving platforms in hybrid mode. There is no
fully decentralized extra-credit mode.

## Build

From the repository root, with the vendored submodules present:

```bash
cmake -S . -B build
cmake --build build --target signal-bloom signal-bloom-server signal-bloom-tests -j 4
ctest --test-dir build --output-on-failure
```

## Run three client-server players

Use four terminals, one process per terminal:

```bash
./build/signal-bloom-server
./build/signal-bloom --name Nova
./build/signal-bloom --name Sol
./build/signal-bloom --name Mira
```

Each client can be launched before or after the server. Set one client to `1`
(0.5x), one to `2` (1x), and one to `3` (2x) to compare their measured loop
rates in the client HUD. The publish-call counter in `F2` diagnostics is not a
wire-message count: the networking layer can skip a send while awaiting a reply.
For a repeatable measurement without key presses, add `--scale 0.5`,
`--scale 1`, or `--scale 2` to the three client commands.

## Run three hybrid peers

Start the same server, then use three more terminals:

```bash
./build/signal-bloom --name Nova --mode peer-to-peer --peer-id 1 --port 7100
./build/signal-bloom --name Sol --mode peer-to-peer --peer-id 2 --port 7200 --bootstrap tcp://127.0.0.1:7100
./build/signal-bloom --name Mira --mode peer-to-peer --peer-id 3 --port 7300 --bootstrap tcp://127.0.0.1:7100
```

Peers exchange player positions and relay claims directly. They make
read-only `GET_WORLD` requests to the server for shuttle positions; they do
not `JOIN` it or consume its player slots. Begin with peer 1, or launch in a
different order and allow discovery time. Do not mix client-server and peer
players in one station session; they use separate player rosters.

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
| `Space` or `W` | Jump |
| Hold `E` | Activate the relay beneath your feet |
| `P` | Pause or resume local game time |
| `1` / `2` / `3` | 0.5x / 1x / 2x game time and loop rate |
| `F1` | Toggle proportional/constant window scaling |
| `F2` | Toggle clock, worker, and publish diagnostics |
| `Esc` | Quit |

The pause is *local*: that astronaut and its game-time effects stop, while
other players and server-authored shuttles continue. The game keeps polling
networking, so it can recover without unpausing. A missing world authority
freezes the last known shuttles and shows a warning; a restarted server resets
its world rather than restoring old progress. The station is online only
while three distinct, connected players hold three different relays.

Useful screenshots for the individual reflection are (1) all three relays
active in three windows, (2) different client loop rates, and (3) hybrid mode
with peers and a shared shuttle.
