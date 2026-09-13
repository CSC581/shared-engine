#pragma once

#include "DeltaTimer.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"

#include <SDL3/SDL_scancode.h>

#include <cstdint>

struct SDL_Renderer;
struct SDL_Window;
class Game;

// Core of the engine: window creation, the renderer, the main loop and the
// rendering scale mode. It holds no game state of its own -- the game it drives
// is supplied to run() as a Game.
class Engine {
public:
    // An RGB colour, used for the screen clear.
    struct Color {
        std::uint8_t red;
        std::uint8_t green;
        std::uint8_t blue;
    };

    // Task 1 asks the loop to clear to blue; a game may pick its own.
    static constexpr Color defaultClearColor{30, 60, 140};

    // How entity coordinates and sizes are mapped onto the window.
    enum class ScaleMode {
        // Pixel-based: one design unit is one pixel, whatever the window size.
        // Resizing the window reveals more (or less) of the world.
        Constant,
        // Aspect-preserving: the design resolution is uniformly scaled to
        // fit the window, with letterboxing where its aspect ratio differs.
        Proportional,
    };

    // width/height are the design resolution the game lays its world out in.
    Engine(const char* title, int width, int height);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // Runs the main loop until the window is closed or quit() is called.
    void run(Game& game);

    // Stop the loop at the end of the current frame.
    void quit();

    SDL_Renderer* getRenderer() const;

    // The window itself, for the few things that need it directly -- an
    // immediate-mode UI backend, a title the game rewrites as state changes.
    SDL_Window* getWindow() const;

    // The timeline everything in the simulation runs on. Pausing or rescaling
    // it freezes or stretches every object whose motion comes from a FrameTime,
    // with no cooperation from those objects. Games bind the keys for that
    // themselves; the engine deliberately reserves none.
    Timeline& gameTime();

    // Real time, never paused and never scaled. Anchor a timeline here for
    // anything that must keep running while the game is frozen -- a menu
    // animation, a "paused" indicator, a network heartbeat.
    const TimeSource& realTime() const;

    // The loop's own delta timer, and so the owner of the stall clamp: how
    // much game time a single frame is allowed to be worth.
    DeltaTimer& frameTimer();

    // Time measured in loop iterations rather than seconds: one tic per pass
    // through the main loop, however long that pass took. Pause, scale and tic
    // size work exactly as they do on the other two. Anchor here for anything
    // that must advance per frame rather than per second -- a fixed-step
    // physics tick, or a lockstep simulation that has to reach the same state
    // on every machine regardless of how fast each one runs.
    Timeline& loopTime();

    // Design resolution the game positions its entities in.
    int getWidth() const;
    int getHeight() const;

    // Colour the screen is cleared to each frame.
    void setClearColor(Color color);
    void setClearColor(std::uint8_t red, std::uint8_t green, std::uint8_t blue);

    ScaleMode getScaleMode() const;
    void setScaleMode(ScaleMode mode);
    void toggleScaleMode();

    // Key that flips between the two scaling modes. Defaults to F1; a game can
    // move it, or disable the built-in toggle with SDL_SCANCODE_UNKNOWN.
    void setScaleToggleKey(SDL_Scancode key);

private:
    // How much game time one frame may be worth before the loop stops
    // believing it. 50 game ms, the value the old float clamp used.
    static constexpr std::int64_t maxFrameDeltaTics = 50'000;

    void applyScaleMode();

    // The engine's clocks, each anchored to the one above it. Declaration
    // order is initialisation order, so it is also the order they depend in:
    // real time, then game time counting it, then the loop's delta on game
    // time. No statics and no singletons -- a second Engine, or a headless
    // server, gets its own set.
    RealTimeClock realClock_;
    Timeline gameTime_{realClock_, kNsPerUs};
    DeltaTimer frameTimer_{gameTime_, maxFrameDeltaTics};

    // The third scale: loop iterations, counted by a clock that does not care
    // how long any of them took.
    FrameClock loopClock_;
    Timeline loopTime_{loopClock_, 1};

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    int width_;
    int height_;

    Color clearColor_ = defaultClearColor;

    ScaleMode scaleMode_ = ScaleMode::Proportional;
    SDL_Scancode scaleToggleKey_ = SDL_SCANCODE_F1;

    bool isRunning_ = true;
};
