#include "Engine.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace {
constexpr char windowTitle[] = "Shared Engine";
}

Engine::Engine()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());
    }

    if (!SDL_CreateWindowAndRenderer(
            windowTitle,
            windowWidth,
            windowHeight,
            SDL_WINDOW_RESIZABLE,
            &window_,
            &renderer_)) {
        SDL_Quit();
        throw std::runtime_error(std::string("Failed to create SDL window and renderer: ") + SDL_GetError());
    }
}

Engine::~Engine()
{
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Engine::run()
{
    Uint64 previousFrameTime = SDL_GetTicks();

    while (isRunning_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                isRunning_ = false;
            }
        }

        const Uint64 currentFrameTime = SDL_GetTicks();
        const float deltaTime = static_cast<float>(currentFrameTime - previousFrameTime) / 1000.0F;
        previousFrameTime = currentFrameTime;

        update(deltaTime);

        SDL_SetRenderDrawColor(renderer_, 0, 0, 255, 255);
        SDL_RenderClear(renderer_);
        render();
        SDL_RenderPresent(renderer_);
    }
}

void Engine::update(float deltaTime)
{
    (void)deltaTime;
}

void Engine::render()
{
}
