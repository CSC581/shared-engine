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
    Uint64 previousFrameTime = SDL_GetTicks();

    while (isRunning_) {
        // The engine only acts on window-close events itself; key events are
        // handed to Input so a key tapped between frames still registers, and
        // all gameplay input is read through the polling Input system.
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                isRunning_ = false;
            }

            Input::handleEvent(event);
        }

        Input::update();

        if (scaleToggleKey_ != SDL_SCANCODE_UNKNOWN && Input::isKeyJustPressed(scaleToggleKey_)) {
            toggleScaleMode();
        }

        game.handleInput(*this);

        const Uint64 currentFrameTime = SDL_GetTicks();
        float deltaTime = static_cast<float>(currentFrameTime - previousFrameTime) / 1000.0F;
        previousFrameTime = currentFrameTime;

        // Keep physics stable if the window stalls for a moment.
        if (deltaTime > maxDeltaTime) {
            deltaTime = maxDeltaTime;
        }

        game.update(deltaTime, *this);

        applyScaleMode();
        SDL_SetRenderDrawColor(renderer_, clearColor_.red, clearColor_.green, clearColor_.blue, 255);
        SDL_RenderClear(renderer_);
        game.render(renderer_);
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
