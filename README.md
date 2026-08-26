# Shared Engine

A small C++17 game-engine foundation built with SDL3.

## Build And Run

From the project folder:

```bash
cmake -S . -B build
cmake --build build
./build/shared-engine
```

The current engine opens a blue SDL window. Close the window to exit.

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

`Physics` provides configurable gravity. Call `Physics::applyGravity(entity, deltaTime)` only for objects that should fall.

The shared update order is:

```text
input -> gravity -> entity update -> collision -> render
```
