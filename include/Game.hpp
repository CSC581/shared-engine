#pragma once

struct SDL_Renderer;
class Engine;

// The engine's extension point: everything a specific game plugs in.
//
// The engine owns the window, the renderer and the frame loop; it knows nothing
// about players, enemies or scores. Each individual game subclasses Game and
// hands an instance to Engine::run().
class Game {
public:
    virtual ~Game() = default;

    // Called once per frame, after Input::update(), before update().
    virtual void handleInput(Engine& engine) = 0;

    // Advance the simulation. deltaTime is in seconds and already clamped.
    virtual void update(float deltaTime, Engine& engine) = 0;

    // Draw the frame. The engine has already cleared the screen and applied the
    // current scaling mode, and presents once this returns.
    virtual void render(SDL_Renderer* renderer) const = 0;
};
