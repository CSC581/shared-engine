# Shared Engine

A small C++17 game-engine foundation built with SDL3.

`Engine` owns the SDL window, renderer, main loop and render scaling; it holds
no gameplay state of its own. A game is a class that implements the `Game`
interface (`handleInput` / `update` / `render`) and is handed to
`Engine::run()`. `Entity`, `Physics`, `Collision` and `Input` are the shared
toolkit every game is built from.

## Milestone 1 Coverage

| Milestone task | Shared-engine implementation | Where to find it |
| --- | --- | --- |
| 1. Core graphics setup | Initializes SDL3, creates a window and renderer, runs the event/update/render loop, clears to blue, presents each frame, and cleans up SDL resources. Windows are resizable. | `include/Engine.hpp`, `src/Engine.cpp` |
| 2. Entity system | A generic `Entity` stores position, width, height, and velocity. It can update its position from velocity and expose a bounding rectangle. | `include/Entity.hpp`, `src/Entity.cpp` |
| 3. Physics | `Physics::setGravity()` configures gravity and `Physics::applyGravity()` adds downward velocity only to entities selected by the game. | `include/Physics.hpp`, `src/Physics.cpp` |
| 4. Input | Uses `SDL_GetKeyboardState` once per frame and provides held, just-pressed, and just-released key queries. | `include/Input.hpp`, `src/Input.cpp` |
| 5. Collision | Generic axis-aligned bounding-box (AABB) overlap detection works on `Entity` or `Rect` values and returns whether objects overlap. Additional helpers support game-specific responses. | `include/Collision.hpp`, `src/Collision.cpp` |
| 6. Scaling (CSC 581) | Provides constant/pixel scaling and proportional scaling, with `F1` toggling between them while a game runs. | `include/Engine.hpp`, `src/Engine.cpp` |

`games/CoinRunner.cpp` constructs a `1920 x 1080` window, which demonstrates
the required window size. The engine also accepts a title and dimensions in
its constructor so individual games can choose their own settings without
editing shared-engine files.

## Architecture

The engine and games have separate responsibilities:

```text
Engine     -> SDL setup, window, renderer, loop, timing, clear, present, scaling
Game       -> game-specific input handling, rules, objects, rendering, responses
Entity     -> position, size, velocity, and movement from velocity
Physics    -> configurable gravity
Input      -> keyboard state queries
Collision  -> geometry and overlap/separation calculations
```

The intended frame order is:

```text
input -> gravity (selected entities) -> entity update -> collision -> render
```

The engine detects only the window-close event. Games use `Input` to read
keyboard state and decide their own gameplay behavior. Similarly, `Collision`
reports geometry facts; each game decides whether an overlap means landing,
losing a life, collecting an item, or something else.

## Build And Run

`vendored/SDL` is a git submodule, so first-time clones need it fetched before
configuring:

```bash
git submodule update --init --recursive
```

Then, from the project folder:

```bash
cmake -S . -B build
cmake --build build
```

The first configure also builds the vendored SDL, so it takes a few minutes;
rebuilds after that are quick.

Each example game is its own executable in `build/`:

```bash
./build/coin-game    # Coin Runner: platformer, collect coins, avoid a patrol
./build/catch-game   # dodge falling obstacles, catch falling coins
./build/siege-game   # Angry-Birds-style launcher
./build/slice-game   # Fruit-Ninja-style blade slicer
./build/beetle-game  # dung beetle rolling and jumping
./build/golf-game    # side-on golf with a scrolling camera
```

All six link the same engine sources (`src/Engine.cpp`, `src/Input.cpp`,
`src/Collision.cpp`, `src/Entity.cpp`, `src/Physics.cpp`) and exist to
exercise the shared engine, not as separate products.

## Scaling Demonstration

Run Coin Runner and press `F1` to switch between the two required modes. Resize
the window after switching so the difference is visible.

| Mode | Behavior |
| --- | --- |
| Constant / pixel scaling | One game unit is one screen pixel. Resizing the window shows more or less of the game world. |
| Proportional scaling | The game's design resolution scales by one uniform factor. The image keeps its aspect ratio; unused space may appear at the sides or top and bottom. |

The scaling key can also be changed per game with
`Engine::setScaleToggleKey()` if an individual game needs a different control.

## Run Tests

```bash
ctest --test-dir build --output-on-failure
```

## Entity And Physics

`Entity` represents a game object with a position, size, and velocity.

- `update(deltaTime)` moves the entity using its velocity.
- `getBounds()` returns its position and size as a `Rect` for collision detection.
- `setVelocityX()` and `setVelocityY()` change one movement axis without affecting the other.
- `setSize()` supports resizing an existing entity.
- `collidesWith(other)` answers "am I overlapping that?" directly on the entity.
- `containsPoint(x, y)` tests a point against the entity.

The two collision helpers are declared on `Entity` but implemented in
`Collision.cpp`, so `Entity` itself carries no dependency on the collision
system.

`Physics` provides configurable gravity. Call
`Physics::applyGravity(entity, deltaTime)` only for objects that should fall.
This makes gravity opt-in: platforms, flying enemies, and UI objects do not
fall unless the game explicitly applies it to them.

## Collision

`Collision` is the engine's bounding-box (AABB) collision system. It answers
questions about geometry only — deciding what happens after a hit is the game's
job.

```cpp
if (player.collidesWith(hazard)) {
    player.setPosition(startX, startY);  // the game's response
}
```

- `Collision::intersects(a, b)` — do two entities (or two `Rect`s) overlap?
  Edge-touching does not count, and zero-sized boxes never collide.
- `Collision::getIntersection(a, b)` — the overlapping region as a `Rect`, for
  games that care how deep the overlap is.
- `Collision::contains(box, x, y)` — is a point inside the box?
- `Collision::getSeparation(moving, blocker, outX, outY)` — the smallest push
  along a single axis that separates the two. Returns `false` when they are not
  overlapping. The push axis tells the game *how* it was hit: a negative `outY`
  means the entity landed on top of the blocker, which is how a platformer
  detects being grounded.
- `Collision::resolve(moving, blocker)` — convenience response that pushes
  `moving` out of `blocker` and zeroes velocity on the blocked axis only.

`Collision` has no SDL dependency, so it can be used and unit-tested on its own.

## Input

`Input` is a keyboard state system. The engine calls `Input::update()` once per
frame; a game only ever asks whether a key is down. Nothing goes through the SDL
event queue, which is used only for the window-close event.

```cpp
if (Input::isKeyPressed(SDL_SCANCODE_W)) {
    player.setVelocityY(-speed);
}
```

- `isKeyPressed(key)` — the key is currently held.
- `isKeyJustPressed(key)` — the key went down this frame (jump, shoot, menus).
- `isKeyJustReleased(key)` — the key came up this frame.
- `setKeyboardStateSource(state, count)` — test hook that swaps in a fake
  keyboard array; pass `nullptr` to return to the real hardware.

Multiple keys read independently, so diagonal movement and opposing keys that
cancel out both work.

## Sample Game: Coin Runner

`games/CoinRunner.cpp` is a small platformer that shows how a game uses the
engine. It is a self-contained file: the `CoinRunner` class, its game logic
and its own `main()`.

```bash
./build/coin-game
```

| Key | Action |
| --- | --- |
| `A` / `D` or arrow keys | Move left and right |
| `Space` | Jump (only while standing on something) |
| `F1` | Toggle render scaling mode |
| `Esc` | Quit |

Collect the gold coins, avoid the red patrol that walks the ground. You have
three lives; score and lives are drawn in the corner and collision events are
printed to the terminal.

The game's collision responses live in `CoinRunner::handleCollisions()`
(`games/CoinRunner.cpp`) and are written by the game, not the engine — the
engine's `Collision` system only answers "do these overlap?":

- **Grey platforms** stop the player. A downward-blocked hit also marks the
  player as grounded, which is what allows the next jump.
- **Red patrol** costs a life and resets the player to the start. At zero lives
  the game ends.
- **Gold coin** increases the score and respawns at the next spot.

It exercises every engine system: `Input` for movement and edge-triggered
jumping, `Physics` for gravity, `Entity` for every object in the level, and
`Collision` for all three responses above.

The other five games (`catch-game`, `siege-game`, `slice-game`,
`beetle-game`, `golf-game`) are further, more advanced exercises of the same
engine — they are not separate submissions, just proof that the shared code
is reusable across different kinds of games.

## Submission Checklist

### Team Shared-Engine Submission

- Include the reusable engine source and headers for Tasks 1 through 6.
- Include this README as the engine design documentation and task-to-code map.
- Verify a clean clone can initialize the SDL submodule, configure with CMake,
  build, and run the test suite using the commands above.
- Demonstrate the blue render loop, input, entity updates, gravity, collision,
  and both scaling modes with an example game.

### Individual Game Submission

Each person creates a separate game project that uses this shared engine. That
project should have its own `main.cpp`: it creates an `Engine`, creates that
person's `Game` class, and calls `engine.run(game)`.

- Include at least one static object, one player-controlled object, and one
  automatically moving object.
- Decide which objects receive `Physics::applyGravity()`.
- Use the shared `Input` interface for controls.
- Implement game-specific collision responses using the shared `Collision`
  results.
- Demonstrate the required scaling behavior if it is part of the course level.
- Submit the required individual writeup/reflection separately; this README
  documents the team engine and does not replace that writeup.
