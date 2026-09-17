# Apex Ascent (harsha)

Jump King–style vertical climber on the shared engine: charge a jump, aim,
release, and climb a tower of platforms.

## Controls

| Input | Action |
| --- | --- |
| A / D or Left / Right | Walk (grounded) or aim while charging |
| Space (hold / release) | Charge jump / leap |
| F1 | Toggle `Proportional` / `Constant` scale |
| Esc | Quit |

## Build and run

```bash
cmake -S . -B build
cmake --build build --target apex-ascent
./build/apex-ascent
```

CMake copies player PNGs next to the binary (`build/media/apex-ascent/`). Run
the executable from any cwd; assets resolve via `SDL_GetBasePath()`.

## Layout

| Path | Role |
| --- | --- |
| [apexAscent.cpp](apexAscent.cpp) | Gameplay, level, camera, collision, HUD |
| [animation/](animation/) | `PlayerAnimation` — sheet load, clips, draw |
| [media/Spritesheets/Spritesheets/](media/Spritesheets/Spritesheets/) | Source penguin spritesheets |
| [docs/individual-games/harsha/apex-ascent-design.md](../../docs/individual-games/harsha/apex-ascent-design.md) | Design decisions |

## Credits

**Player Sprites & Animations:**

- **Asset Pack:** FREE Animated Pixel Art Penguin
- **Artist:** Duckhive
- **Source:** [https://duckhive.itch.io/penguin](https://duckhive.itch.io/penguin)
- **Itch.io Game ID:** 2441567
