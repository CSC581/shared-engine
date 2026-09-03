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
