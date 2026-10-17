# Apex Ascent (harsha)

Jump King–style vertical climber on the shared engine: charge a jump, aim,
release, and climb a tower of platforms. Optional online co-climb uses the
shared `Multiplayer::Session` interface in either client-server or peer-to-peer
mode.

## Controls

| Input | Action |
| --- | --- |
| A / D or Left / Right | Walk on the ground, aim while charging, steer in the air |
| Space (hold / release) | Charge jump / leap |
| P | Pause / unpause this client's game time (movement and jumps are ignored while paused) |
| 1 / 2 / 3 | 0.5× / 1× / 2× game time |
| F1 | Toggle `Proportional` / `Constant` scale |
| Esc | Quit |

Holding A / D in the air sets your sideways speed to walking speed; letting go
keeps whatever speed the jump had. Steering can change direction but never
adds height.

## The tower

Six rooms, one screen each. The camera snaps to whichever room you are in.

| Room | What it asks of you |
| --- | --- |
| 0 Tutorial | Stone ledges for short, medium and sideways charges; an amber mover for timing |
| 1 Lift | Wide sandstone landing; the elevator is the only way up to the narrow moss ledges |
| 2 Ice | Slippery ice ledges; land short of the top one, or slide off it, and the spike shelf below sends you back |
| 3 Slingshot | The goal is high above the fast magenta rail and only a little to the left. Charge fully, aim nowhere, and release while the rail moves left: the rail throws you there. A straight-up jump steered left in mid-air also works, but the throw is the intended route |
| 4 Orbit | Three violet wheels at 3:2:1 speeds, repeating every 8 s; land on a moving target |
| 5 Summit | Narrow ledges in gusty wind (watch the streaks and the HUD arrow) up to the gold summit |

Platforms: slate stone, green moss (narrow), tan sandstone (wide), pale cyan
ice (slippery), amber movers, magenta slingshot rail (on a drawn track),
violet orbit wheels (on drawn circles), gold summit.

Hidden objects, all triggers with nothing drawn for the trigger itself:
four starting slots on the ground, a checkpoint on each room's first ledge
(landing on it makes it your respawn point), death zones (the spikes in
room 2, drawn as a cue, and a failsafe below the world), one room zone per
room for the camera, and the wind zone over room 5.

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
./build/apex-ascent --join          # repeat in separate terminals (up to 4 slots)
```

Each climber starts on its own ground slot, `(player id - 1) % 4`.

Use `./build/apex-network-server` (tower world), not the sandbox
`network-server` (arena demo). Rebuild server and clients together after a
protocol bump. Defaults: `tcp://*:5555` / `tcp://127.0.0.1:5555`.

Across machines, pass the server's reachable host separately from its bind:

```bash
./build/apex-network-server 'tcp://*:5555' --advertise 192.168.1.10
./build/apex-ascent --join tcp://192.168.1.10:5555
```

Online: other climbers are tinted animated ghost sprites, and all six moving
platforms (mover, elevator, slingshot rail, three orbit wheels) are
server-authored. Every client mirrors them into its own `World` through
`NetworkWorldSync`, so carry and the slingshot throw work exactly as
offline. Ice, wind, air steering and the throw are applied by each client
to its own climber. Pause and scale only affect your local climb.

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
| [ApexGame.hpp](include/ApexGame.hpp) / [ApexGame.cpp](src/ApexGame.cpp) | Gameplay on a `World`: frame order across the two workers, input, charge jump, landing pass, carry, slingshot throw, ice, wind, checkpoints, respawn, room-snap camera, pose publishing |
| [ApexGameRender.cpp](src/ApexGameRender.cpp) | `renderWorld` for platforms, rail track and orbit guides, spikes, wind streaks, ghosts, charge meter, HUD |
| [ApexComponents.hpp](include/ApexComponents.hpp) / [ApexComponents.cpp](src/ApexComponents.cpp) | Apex components: `ApexPlatform`, `Surface`, `Carrier`, `SpawnMarker`, `DeathZone`, `RoomZone`, `WindZone` |
| [ApexLevel.hpp](include/ApexLevel.hpp) / [ApexLevel.cpp](src/ApexLevel.cpp) | Factory per object kind (platforms, movers, player, online objects, hidden triggers) and one function per room |
| [ApexOptions.hpp](include/ApexOptions.hpp) / [ApexOptions.cpp](src/ApexOptions.cpp) | Command line (`--join`, `--mode`, `--host`, …) |
| [ApexPresence.hpp](include/ApexPresence.hpp) / [ApexPresence.cpp](src/ApexPresence.cpp) | Online presentation: HUD labels, ghost tints, animation hint encode/decode |
| [ApexNetworkConfig.hpp](include/ApexNetworkConfig.hpp) | The one mover table (`movers()`) and the spawn slots, shared by the offline level, the online factory and the server |
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
