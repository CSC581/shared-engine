# Shared Engine

A small C++17 game-engine foundation built with SDL3.

`Engine` owns SDL setup, the window, renderer, main loop, timing, and render
scaling. A game implements `Game` (`handleInput`, `update`, and `render`) and
is passed to `Engine::run()`. `Entity`, `Physics`, `Input`, and `Collision`
provide the reusable game-building tools.

## Milestone 1 Coverage

| Milestone task | Shared-engine implementation | Where to find it |
| --- | --- | --- |
| 1. Core graphics setup | Initializes SDL3, creates a resizable window and renderer, runs the game loop, clears the screen blue, presents each frame, and cleans up SDL resources. | `include/Engine.hpp`, `src/Engine.cpp` |
| 2. Entity system | A generic `Entity` stores position, width, height, and velocity. It updates its position and exposes a bounding rectangle. | `include/Entity.hpp`, `src/Entity.cpp` |
| 3. Physics | `Physics::setGravity()` configures gravity and `Physics::applyGravity()` adds downward velocity to selected entities. | `include/Physics.hpp`, `src/Physics.cpp` |
| 4. Input | Uses `SDL_GetKeyboardState` and provides held, just-pressed, and just-released key queries. | `include/Input.hpp`, `src/Input.cpp` |
| 5. Collision | Generic axis-aligned bounding-box (AABB) overlap detection works on `Entity` or `Rect` values. | `include/Collision.hpp`, `src/Collision.cpp` |
| 6. Scaling (CSC 581) | Provides constant/pixel scaling and proportional scaling. `F1` toggles between modes by default. | `include/Engine.hpp`, `src/Engine.cpp` |

## Architecture

```text
Engine     -> SDL setup, window, renderer, loop, timing, clear, present, scaling
Game       -> game-specific rules, objects, input handling, and rendering
Entity     -> position, size, velocity, and movement
Physics    -> configurable gravity
Input      -> keyboard state queries
Collision  -> overlap and separation calculations
Timeline   -> pausable, rescalable clocks the simulation runs on
DeltaTimer -> per-consumer "time since I last looked"
```

The intended order for each frame is:

```text
input -> gravity (selected entities) -> entity update -> collision -> render
```

## Build

`vendored/SDL` is a Git submodule. On a new clone, fetch it first:

```bash
git submodule update --init --recursive
```

Configure and build from the project folder:

```bash
cmake -S . -B build
cmake --build build
```

Run the automated checks with:

```bash
ctest --test-dir build --output-on-failure
```

## Key Features

### Entity And Physics

An `Entity` has a position, size, and velocity. Calling
`entity.update(deltaTime)` moves it using that velocity.

```cpp
Entity player(100.0F, 200.0F, 32.0F, 32.0F);
player.setVelocity(200.0F, 0.0F);
player.update(deltaTime);
```

Gravity is opt-in. A game chooses which objects should fall:

```cpp
Physics::setGravity(980.0F);
Physics::applyGravity(player, deltaTime);
```

### Input

The engine updates keyboard state once each frame. Games can query keys without
reading SDL events directly.

```cpp
if (Input::isKeyPressed(SDL_SCANCODE_A)) {
    player.setVelocityX(-speed);
}

if (Input::isKeyJustPressed(SDL_SCANCODE_SPACE)) {
    // Start a jump, shoot, or open a menu.
}
```

Available queries are `isKeyPressed`, `isKeyJustPressed`, and
`isKeyJustReleased`.

### Collision

Collision is SDL-free AABB geometry. It reports whether rectangles overlap;
the game decides the response.

```cpp
if (player.collidesWith(hazard)) {
    // The game decides what a hazard collision means.
}
```

`Collision::resolve(moving, blocker)` is available when a moving entity should
be pushed out of a platform or wall. `getIntersection` and `getSeparation`
provide more detailed collision information when needed.

### Scaling

Press `F1` while a game is running, then resize the window to compare modes.

| Mode | Behavior |
| --- | --- |
| Constant / pixel scaling | One game unit equals one screen pixel. Resizing can reveal more or less of the game world. |
| Proportional scaling | The design resolution scales uniformly and keeps its aspect ratio. Unused space may appear at the sides or top and bottom. |

Use `Engine::setScaleToggleKey()` to change or disable the default `F1` key.

### Timelines

The engine measures time on three scales, and owns a clock for each:

| Scale | Clock | Counts |
| --- | --- | --- |
| Real time | `realTime()` | Nanoseconds off `steady_clock`. Never pauses, never scales. |
| Game time | `gameTime()` | Game microseconds. Pausable and rescalable; the simulation runs on this. |
| Loop iterations | `loopTime()` | One tic per pass through the main loop, however long that pass took. |

Each frame the engine builds a `FrameTime` off the game timeline -- game
seconds elapsed, plus absolute game time -- and hands it down.

Everything whose motion comes from that `FrameTime` can therefore be frozen or
stretched by one call, with no cooperation from any of it:

```cpp
engine.gameTime().togglePause();  // freezes everything on game time at once
engine.gameTime().setScale(0.5);  // half speed, with no jump in position
```

Entities stay time-agnostic -- they own no clock and ask none what time it is,
they are simply told how far to move:

```cpp
void MyGame::update(const FrameTime& time, Engine&)
{
    player_.update(time);                                  // velocity * dt
    const double t = time.gameTimeUs / 1'000'000.0;        // absolute, no drift
    platform_.setPosition(originX + amplitude * std::sin(t), platformY);
}
```

A game that only implements the older `update(float deltaTime, Engine&)` keeps
working untouched; the engine forwards to it.

Timelines nest. Anchoring one to another gives a clock that inherits the
parent's pauses and multiplies its scale, which is what a slow-motion layer or
a per-client loop speed is made of:

```cpp
Timeline childTime(engine.gameTime(), 1000);  // anchor, tics of the anchor
DeltaTimer childTimer(childTime, 250);        // one per consumer
```

`engine.realTime()` is never paused and never scaled -- anchor to it for menu
animation or anything that has to keep running while the game is frozen. Input
polling and rendering run on real time for the same reason: a loop that waited
on a paused timeline could never read the key that unpauses it.

`engine.loopTime()` counts frames rather than seconds, and takes the same
pause, tic size and scale as the others -- a tic size of 2 is one tic every
second iteration. Anchor to it when a simulation has to advance per frame
rather than per second and reach the same state on every machine however fast
each one runs: lockstep peer-to-peer sync, a reproducible replay, a fixed-step
physics tick.

The module (`TimeSource`, `Timeline`, `DeltaTimer`, `FrameTime`) has no SDL
dependency and no global state, so a headless server can link `engine-time` on
its own. Every `Timeline` method is thread-safe.

### The Timeline Sandbox

`timeline-sandbox` is an interactive bench for all of the above: pause, scale
and tic size sliders, a child timeline, an adjustable frame-delta clamp and
frame delay, and a graph of recent frame deltas.

```bash
cmake --build build --target timeline-sandbox
./build/timeline-sandbox            # --frames N runs N frames and exits
```

`P` pauses, `1`/`2`/`3` select 0.5x, 1.0x and 2.0x, `WASD` moves. It is the only
target that depends on Dear ImGui; the engine library must not.

### Multiple Keys At Once

`Input::update()` copies the whole keyboard every frame (`src/Input.cpp`), so
every key is an independent slot and any number of them can read as held in the
same frame. Diagonal movement, run-while-jumping, modifier combos and two
players sharing one keyboard all work without special handling.

Helpers for reading several keys together:

- `areAllKeysPressed({a, b, c})` — every listed key is held right now, for
  chords and modifier combos.
- `isAnyKeyPressed({a, b})` — at least one of them is held.
- `getAxis(negativeKey, positiveKey)` — `-1` / `0` / `+1` for an opposed pair;
  holding both cancels out. Two calls give a full 8-way direction.
- `pressedKeyCount()` and `getPressedKeys()` — how many, and which, keys are
  held this frame. Useful for debug readouts.

```cpp
// 8-way movement plus a sprint modifier: up to three keys at once.
const float speed = Input::isKeyPressed(SDL_SCANCODE_LSHIFT) ? sprintSpeed : walkSpeed;
player.setVelocity(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D) * speed,
                   Input::getAxis(SDL_SCANCODE_W, SDL_SCANCODE_S) * speed);
```

A very brief press and release that happens entirely between two frames may not
be visible to a polling input system. If a specific physical combination never
appears in `getPressedKeys()`, that is keyboard ghosting in the hardware rather
than an engine limitation; most non-gaming keyboards drop the third
simultaneous key in some rows.
