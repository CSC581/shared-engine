# Frostbrew Duel

Two penguin alchemists share one keyboard, mixing and throwing potions across
a changing snowfield. Reduce the opponent's 100 health to zero. Projectiles can
hurt either player; a double knockout is a draw. Penguins stay at fixed horizontal
positions, but terrain growth lifts them and excavation lowers them.

## Build and play

```bash
cmake -S . -B build
cmake --build build --target frostbrew-duel -j 4
./build/frostbrew-duel
```

Assets resolve beside the executable, so launching from another directory works.

| Input | Action |
| --- | --- |
| 1 / 2 / 3 | Place Ember / Stone / Growth in the highlighted ingredient slot, then select the other slot |
| Tab | Change the highlighted ingredient slot |
| Up / Down | Raise / lower firing angle |
| Left / Right | Lower / raise throwing power |
| Up + Down | Hold focus to see the predicted trajectory; angle adjustment stops |
| Space | Throw the mixture; the turn waits for all fragments or seeds |
| B | Open / close the active player's recipe book |
| P | Pause / continue |
| R | Restart the match, including discoveries |
| Esc | Quit |

Ingredient order does not matter. Both players know Blast Flask initially.
Other recipes are revealed to the thrower only after a successful launch.
Ingredients are unlimited. The game starts with two Ember ingredients selected.

| Ingredients | Potion | Effect |
| --- | --- | --- |
| Ember + Ember | Blast Flask | Crater and splash damage |
| Ember + Stone | Shatter Flask | Eight explosive fragments |
| Ember + Growth | Bloom Bomb | Eight terrain-growing seeds |
| Stone + Stone | Deep Flask | Narrow, deep crater and concentrated damage |
| Stone + Growth | Earthbrew | Tall snowbank |
| Growth + Growth | Garden Flask | Wide snowbank |

## Engine reuse

The game links the existing engine without changing engine source or headers:

| Existing code | Use in this game |
| --- | --- |
| `Engine`, `Game`, proportional rendering | Window, loop, renderer and resizing |
| `World`, `GameObject`, components | Penguin, flask and effect ownership, identity and lifetime |
| `World(128)`, `tryCreate`, deferred destruction | Fixed GameObject capacity and repeated slot reuse |
| `Motion`, `Gravity`, `FrameTime` | Live projectile/particle motion and prediction in a separate one-slot pooled World |
| `Collider`, `World::resolveCollisions`, `Collision` | Trigger contact, collision events and rectangle checks along flight paths |
| `EventManager`, engine input events | Input, collision and queued impact delivery |
| `registerChord` | Focus action using the two existing angle keys |
| Engine game timeline | Pause and elapsed game time |
| `Renderable`, `renderWorld` | Flask/particle rendering and layers |

Height-map terrain, surface contact, recipes, discovery, damage and turns are
game rules. They live in `FrostbrewTerrain`, `FrostbrewRules` and `FrostbrewArena`.
`FrostbrewGame` connects these to engine input and presentation. Ordinary
implementations live in cpp files. The engine remains the physics integrator:
both prediction and live flight advance its components at 240 steps/second.

The terrain supports surface-connected craters and mounds, with no tunnels,
caves or overhangs. It is drawn directly from its surface heights, rather than
allocating an object per pixel. Settlement uses the highest terrain under the
whole penguin footprint and does not apply falling damage.

There are at most eight secondary projectiles and 64 cosmetic particles. Flight
expires after eight game seconds. A full pool skips secondary/cosmetic spawns;
failure to allocate a primary flask keeps the turn and recipe undiscovered.
Cosmetics reserve room for gameplay and never delay turn handover. Only
GameObject storage is pooled; components and other containers can still allocate.

## Tests and evidence

```bash
cmake --build build --target frostbrew-rules-tests frostbrew-integration-tests frostbrew-ui-tests frostbrew-duel -j 4
ctest --test-dir build --output-on-failure -R '^frostbrew-'
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./build/frostbrew-duel --smoke
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./build/frostbrew-duel --screenshot build/frostbrew-scene.png
python3 individual-games/harsha/frostbrew-duel/compare_games.py > build/frostbrew-source-comparison.json
```

The smoke run uses scripted aiming and fixed synthetic game deltas through the
real Engine loop. It must reach a terminal match; it is correctness evidence,
not an interactive playtest or performance measurement. UI tests feed the
engine's existing keyboard test source and chord events. Asset tests launch a
copied executable from another working directory and check missing/corrupt art.

The comparison script reports raw source-line matches/differences against the
Apex client, including comments and blanks. Its symmetric changed percentage
is `100 * (old + new - 2 * matching) / (old + new)`. It excludes shared engine,
tests, art and generated files. This is a code-diff statistic, not an engine-reuse
percentage; the table above identifies actual reused systems. Apex's additional
networking and platformer scope make the games different sizes.

## Credits

- Gameplay inspiration: Pocket Tanks by BlitWise; this is an original game implementation.
- Penguin sheets: **FREE Animated Pixel Art Penguin**, Duckhive,
  <https://duckhive.itch.io/penguin>. Source sheets are the existing Apex assets;
  CMake copies them into this game's runtime asset directory. The creator permits
  personal and commercial use.
- Potion icons, scarves, terrain and effects are drawn by the game; no additional
  downloaded asset pack is required.

Initial potion balance and controls still need human playtesting. This is the
second game; CSC581's additional third-game requirement is separate.

## Pool capacity

The 128-slot live World uses a conservative budget: two penguins, one primary
flask, eight fragments/seeds and 64 particles total 75 slots. The primary flask
is actually flushed before its children spawn, so this deliberately overcounts
by one. Even adding nine free slots for the cosmetic spawn reservation gives
84, below 128. Terrain does not consume object slots. Trajectory prediction uses
a separate one-slot World.

The integration tests fill 63 cosmetic slots and split each of Shatter and Bloom
at the default capacity. They check that all eight children and the final
particle fit (74 occupied slots), that the shot resolves, and that restart
returns occupancy to two. Existing tests cover all recipes, primary-shot failure
and child exhaustion in deliberately tiny pools.

This establishes headroom for the current bounded game rules, rather than a
measured optimal capacity or a cap on total game memory. Revisit the budget if
projectile, particle or player limits change.
