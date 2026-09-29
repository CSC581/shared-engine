# Apex Ascent (harsha)

Jump King–style vertical climber on the shared engine: charge a jump, aim,
release, and climb a tower of platforms. Optional online co-climb uses the
shared `Multiplayer::Session` interface in either client-server or peer-to-peer
mode.

## Controls

| Input | Action |
| --- | --- |
| A / D or Left / Right | Walk (grounded) or aim while charging |
| Space (hold / release) | Charge jump / leap |
| F1 | Toggle `Proportional` / `Constant` scale |
| Esc | Quit |

Online only (`--join`):

| Input | Action |
| --- | --- |
| P | Pause / unpause this client's game time (movement and jumps are ignored while paused) |
| 1 / 2 / 3 | 0.5× / 1× / 2× game time |

## Build and run (offline)

```bash
cmake -S . -B build
cmake --build build --target apex-ascent
./build/apex-ascent
```

CMake copies player PNGs next to the binary (`build/media/apex-ascent/`). Run
the executable from any cwd; assets resolve via `SDL_GetBasePath()`.

## Online co-climb — client-server

```bash
cmake --build build --target apex-network-server apex-ascent
./build/apex-network-server
./build/apex-ascent --join          # repeat in separate terminals
```

Use `./build/apex-network-server` (tower world), not the sandbox
`network-server` (arena demo). Rebuild server and clients together after a
protocol bump. Defaults: `tcp://*:5555` / `tcp://127.0.0.1:5555`.

Across machines, pass the server's reachable host separately from its bind:

```bash
./build/apex-network-server 'tcp://*:5555' --advertise 192.168.1.10
./build/apex-ascent --join tcp://192.168.1.10:5555
```

Online: peers are tinted animated ghost sprites; the amber moving platform is
server-authored; pause/scale only affects your local climb.

`./build/apex-network-server --rates` logs each climber's accepted
POSITION/s once a second, so a client at 2x shows up at the server at twice
the rate of one at 1x.

## Online co-climb — peer-to-peer

Player poses and animation state travel directly between peers. The dedicated
server remains authoritative only for the moving platform:

```bash
./build/apex-network-server
./build/apex-ascent --mode peer-to-peer --id 1 --port 7200
./build/apex-ascent --mode peer-to-peer --id 2 --port 7202 \
  --peer tcp://127.0.0.1:7200
./build/apex-ascent --mode peer-to-peer --id 3 --port 7204 \
  --peer tcp://127.0.0.1:7202
```

Alternatively, the first player can carry the platform authority as a
listen-server (the engine's `NetworkServerHost` in Listen mode, inside that
player's process); no separate server process is needed:

```bash
./build/apex-ascent --mode peer-to-peer --id 1 --port 7200 --host
./build/apex-ascent --mode peer-to-peer --id 2 --port 7202 \
  --peer tcp://127.0.0.1:7200
```

Peer ids and base ports must be unique. Across machines, every peer also needs
`--advertise HOST`, and bootstrap addresses must use reachable hosts.

## Layout

| Path | Role |
| --- | --- |
| [apexAscent.cpp](src/apexAscent.cpp) | `main()`: options, optional `--host` listen-server, engine + session |
| [ApexGame.hpp](include/ApexGame.hpp) / [ApexGame.cpp](src/ApexGame.cpp) | Gameplay: level, input, charge jump, collision, camera, moving platforms, pose publishing |
| [ApexGameRender.cpp](src/ApexGameRender.cpp) | World, ghosts, charge meter, HUD |
| [ApexOptions.hpp](include/ApexOptions.hpp) / [ApexOptions.cpp](src/ApexOptions.cpp) | Command line (`--join`, `--mode`, `--host`, …) |
| [ApexPresence.hpp](include/ApexPresence.hpp) / [ApexPresence.cpp](src/ApexPresence.cpp) | Online presentation: HUD labels, ghost tints, animation hint encode/decode |
| [ApexNetworkConfig.hpp](include/ApexNetworkConfig.hpp) | Shared spawn + moving-platform paths for client/server |
| [ApexNetworkServerMain.cpp](src/ApexNetworkServerMain.cpp) | Headless Apex world server: Apex config + CLI on the engine's `NetworkServerHost` |
| [include/animation/](include/animation/), [src/animation/](src/animation/) | `PlayerAnimation` — sheet load, clips, draw, ghost tint |
| [media/Spritesheets/Spritesheets/](media/Spritesheets/Spritesheets/) | Source penguin spritesheets |
| [docs/individual-games/harsha/apex-ascent-design.md](../../docs/individual-games/harsha/apex-ascent-design.md) | Design decisions |

## Credits

**Player Sprites & Animations:**

- **Asset Pack:** FREE Animated Pixel Art Penguin
- **Artist:** Duckhive
- **Source:** [https://duckhive.itch.io/penguin](https://duckhive.itch.io/penguin)
- **Itch.io Game ID:** 2441567
