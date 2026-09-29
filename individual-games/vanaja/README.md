# Starfall Salvage

Starfall Salvage is a three-player space-station game. Collect the four cargo
crates, help each other open the bulkhead, then stand on the three relay pads
and hold `E` together to finish. Avoid the drones and use the moving shuttles
to reach other parts of the station. The player who collected the most cargo
is named the winner.

The same game can run in client-server or hybrid peer-to-peer mode. Both modes
use the game server for the moving shuttles and drones.

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
