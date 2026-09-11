#include "Engine.hpp"

#include "Game.hpp"
#include "Input.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

Engine::Engine(const char* title, int width, int height)
    : width_(width),
      height_(height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());
    }

    if (!SDL_CreateWindowAndRenderer(
            title,
            width_,
            height_,
            SDL_WINDOW_RESIZABLE,
            &window_,
            &renderer_)) {
        SDL_Quit();
        throw std::runtime_error(std::string("Failed to create SDL window and renderer: ") + SDL_GetError());
    }

    applyScaleMode();
}

Engine::~Engine()
{
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Engine::run(Game& game)
{
    // Whatever the game spent building itself is not this frame's delta.
    frameTimer_.reset();

    while (isRunning_) {
        // One tic of the loop clock per pass, counted before anything in the
        // frame happens, so everything below sees the same iteration number.
        loopClock_.advance();

        // Window events keep the application responsive; gameplay input is
        // read exclusively through the polling Input system below.
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // The game sees every event first, and the engine consumes none of
            // them: both this hook and the handling below get every event.
            game.onEvent(event);

            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                isRunning_ = false;
            }
        }

        Input::update();

        if (scaleToggleKey_ != SDL_SCANCODE_UNKNOWN && Input::isKeyJustPressed(scaleToggleKey_)) {
            toggleScaleMode();
        }

        game.handleInput(*this);

        // Ticked after the game has read its input, so a pause pressed this
        // frame takes effect this frame rather than one frame late.
        //
        // Events, input and rendering above and below deliberately run on real
        // time and never consult gameTime_: a loop that waited on a paused
        // timeline could never read the key that unpauses it.
        const std::int64_t deltaTics = frameTimer_.tick();
        const FrameTime frameTime{
            static_cast<double>(deltaTics) / static_cast<double>(kGameTicsPerSecond),
            gameTime_.now()};

        game.update(frameTime, *this);

        applyScaleMode();
        SDL_SetRenderDrawColor(renderer_, clearColor_.red, clearColor_.green, clearColor_.blue, 255);
        SDL_RenderClear(renderer_);
        game.render(renderer_);
        game.renderOverlay(renderer_);
        SDL_RenderPresent(renderer_);
    }
}

void Engine::quit()
{
    isRunning_ = false;
}

SDL_Renderer* Engine::getRenderer() const
{
    return renderer_;
}

SDL_Window* Engine::getWindow() const
{
    return window_;
}

Timeline& Engine::gameTime()
{
    return gameTime_;
}

const TimeSource& Engine::realTime() const
{
    return realClock_;
}

DeltaTimer& Engine::frameTimer()
{
    return frameTimer_;
}

Timeline& Engine::loopTime()
{
    return loopTime_;
}

int Engine::getWidth() const
{
    return width_;
}

int Engine::getHeight() const
{
    return height_;
}

void Engine::setClearColor(Color color)
{
    clearColor_ = color;
}

void Engine::setClearColor(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    clearColor_ = Color{red, green, blue};
}

Engine::ScaleMode Engine::getScaleMode() const
{
    return scaleMode_;
}

void Engine::setScaleMode(ScaleMode mode)
{
    scaleMode_ = mode;
}

void Engine::toggleScaleMode()
{
    scaleMode_ = (scaleMode_ == ScaleMode::Constant) ? ScaleMode::Proportional : ScaleMode::Constant;
}

// Games always draw in design-resolution coordinates; this is the one place
// that decides how those coordinates become pixels.
void Engine::applyScaleMode()
{
    SDL_SetRenderScale(renderer_, 1.0F, 1.0F);

    if (scaleMode_ == ScaleMode::Constant) {
        SDL_SetRenderLogicalPresentation(
            renderer_, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        return;
    }

    SDL_SetRenderLogicalPresentation(
        renderer_, width_, height_, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

void Engine::setScaleToggleKey(SDL_Scancode key)
{
    scaleToggleKey_ = key;
}
