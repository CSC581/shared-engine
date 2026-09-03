# Shared Engine

A small C++17 game-engine foundation built with SDL3.

`Engine` owns the window, renderer, main loop and render scaling; it holds no
gameplay state of its own. A game is a class that implements the `Game`
interface (`handleInput` / `update` / `render`) and is handed to
`Engine::run()`. `Entity`, `Physics`, `Collision` and `Input` are the shared
toolkit every game is built from. See
[docs/OPTIMIZATION_GUIDE.md](docs/OPTIMIZATION_GUIDE.md) for a fuller
architecture walkthrough.

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
rebuilds after that are quick. See
[docs/SETUP.md](docs/SETUP.md) for prerequisites, troubleshooting and a more
detailed walkthrough.

Each example game is its own executable in `build/`:

```bash
./build/coin-game    # Coin Runner: platformer, collect coins, avoid a patrol
./build/catch-game   # dodge falling obstacles, catch falling coins
./build/siege-game   # Angry-Birds-style launcher
./build/slice-game   # Fruit-Ninja-style blade slicer
./build/beetle-game  # dung beetle rolling and jumping
./build/golf-game    # side-on golf with a scrolling camera
./build/chord-game   # two-player multi-key input demo
```

All seven link the same engine sources (`src/Engine.cpp`, `src/Input.cpp`,
`src/Collision.cpp`, `src/Entity.cpp`, `src/Physics.cpp`) and exist to
exercise the shared engine, not as separate products.

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

`Physics` provides configurable gravity. Call `Physics::applyGravity(entity, deltaTime)` only for objects that should fall.

The shared update order is:

```text
input -> gravity -> entity update -> collision -> render
```

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
frame; a game only ever asks whether a key is down, never handling SDL events
itself.

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

Polling alone would miss a key that is pressed *and* released between two
frames, which makes fast combos feel dropped. To close that gap the engine's
main loop forwards key events to `Input::handleEvent()` (`src/Engine.cpp`), and
`update()` merges those latched presses into the frame's state — so a tap that
brief is still reported for one frame and still fires `isKeyJustPressed`.

If a specific physical combination never appears in `getPressedKeys()`, that is
keyboard ghosting in the hardware rather than an engine limitation; most
non-gaming keyboards drop the third simultaneous key in some rows.

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
