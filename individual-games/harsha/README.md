# Apex Ascent (harsha)

Jump King–style vertical climber on the shared engine: charge a jump, aim,
release, and climb a tower of platforms. Optional online co-climb uses the
shared `network-core` module with a dedicated Apex server.

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
| P | Pause / unpause this client's game time |
| 1 / 2 / 3 | 0.5× / 1× / 2× game time |

## Build and run (offline)

```bash
cmake -S . -B build
cmake --build build --target apex-ascent
./build/apex-ascent
```

CMake copies player PNGs next to the binary (`build/media/apex-ascent/`). Run
the executable from any cwd; assets resolve via `SDL_GetBasePath()`.

## Online co-climb

```bash
cmake --build build --target apex-network-server apex-ascent
./build/apex-network-server
./build/apex-ascent --join          # each player, separate terminal
```

Use `./build/apex-network-server` (tower world), not the sandbox
`network-server` (arena demo). Rebuild server and clients together after a
protocol bump. Defaults: `tcp://*:5555` / `tcp://127.0.0.1:5555`.

Online: peers are tinted Idle ghost sprites; the amber moving platform is
server-authored; pause/scale only affects your local climb.

## Layout

| Path | Role |
| --- | --- |
| [apexAscent.cpp](apexAscent.cpp) | Gameplay, level, camera, collision, HUD, online mode |
| [ApexNetworkConfig.hpp](ApexNetworkConfig.hpp) | Shared spawn + moving-platform paths for client/server |
| [ApexNetworkServerMain.cpp](ApexNetworkServerMain.cpp) | Headless Apex world server |
| [animation/](animation/) | `PlayerAnimation` — sheet load, clips, draw, ghost tint |
| [media/Spritesheets/Spritesheets/](media/Spritesheets/Spritesheets/) | Source penguin spritesheets |
| [docs/individual-games/harsha/apex-ascent-design.md](../../docs/individual-games/harsha/apex-ascent-design.md) | Design decisions |

## Credits

**Player Sprites & Animations:**

- **Asset Pack:** FREE Animated Pixel Art Penguin
- **Artist:** Duckhive
- **Source:** [https://duckhive.itch.io/penguin](https://duckhive.itch.io/penguin)
- **Itch.io Game ID:** 2441567
