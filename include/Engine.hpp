#pragma once

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
    static constexpr float maxDeltaTime = 0.05F;

    void applyScaleMode();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    int width_;
    int height_;

    Color clearColor_ = defaultClearColor;

    ScaleMode scaleMode_ = ScaleMode::Proportional;
    SDL_Scancode scaleToggleKey_ = SDL_SCANCODE_F1;

    bool isRunning_ = true;
};
