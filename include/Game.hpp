#pragma once

#include "FrameTime.hpp"

#include <SDL3/SDL_events.h>

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

    // Called for every event the loop polls, before the engine acts on it. The
    // engine consumes nothing, so a game can layer an event-driven system (a
    // debug UI, text entry) on top without taking the polling Input system
    // over. Optional: a game that only wants polled input ignores it.
    virtual void onEvent(const SDL_Event& event) { (void)event; }

    // Called once per frame, after Input::update(), before update().
    virtual void handleInput(Engine& engine) = 0;

    // Advance the simulation. deltaTime is in seconds and already clamped.
    virtual void update(float deltaTime, Engine& engine) = 0;

    // The same call with the frame's full time information: seconds elapsed
    // plus the absolute game time, both on the engine's game timeline. This is
    // the one the engine actually calls; it forwards to the seconds-only
    // version above, so a game that does not care about absolute time or about
    // pausing never has to know this exists.
    //
    // Override this one to read gameTimeUs (motion computed from absolute time
    // never drifts) or to pass the FrameTime down to Entity::update. Note that
    // declaring either overload in a subclass hides the other for calls made
    // through that subclass; add `using Game::update;` if you need both by
    // name. Dispatch through a Game& -- which is all the engine ever does -- is
    // unaffected.
    virtual void update(const FrameTime& time, Engine& engine)
    {
        update(static_cast<float>(time.dtSeconds), engine);
    }

    // Draw the frame. The engine has already cleared the screen and applied the
    // current scaling mode.
    virtual void render(SDL_Renderer* renderer) const = 0;

    // Drawn after render() and before the frame is presented, with the render
    // scale left exactly as render() finished with it. Non-const and optional:
    // it exists for overlays whose draw call mutates their own state, such as
    // an immediate-mode debug UI, which otherwise has nowhere to put itself.
    virtual void renderOverlay(SDL_Renderer* renderer) { (void)renderer; }
};
