#include "animation/PlayerAnimation.hpp"

#include <SDL3/SDL.h>

#include <iostream>
#include <string>

namespace {

constexpr const char* mediaSubdir = "media/apex-ascent/";
constexpr const char* idleFileName = "Idle.png";

} // namespace

bool PlayerAnimation::smokeProbeMedia(SDL_Renderer* /*renderer*/) const
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        std::cerr << "PlayerAnimation: SDL_GetBasePath failed: " << SDL_GetError() << '\n';
        return false;
    }

    // SDL3 caches this path; do not SDL_free it (unlike SDL2).
    const std::string idlePath = std::string(basePath) + mediaSubdir + idleFileName;

    SDL_Surface* surface = SDL_LoadPNG(idlePath.c_str());
    if (surface == nullptr) {
        std::cerr << "PlayerAnimation: could not load \"" << idlePath
                  << "\": " << SDL_GetError() << '\n';
        return false;
    }

    std::cout << "PlayerAnimation: found media at \"" << idlePath << "\" ("
              << surface->w << 'x' << surface->h << ")\n";
    SDL_DestroySurface(surface);
    return true;
}
