#include "ArcadeDraw.hpp"

#include "World.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

namespace Arcade {

namespace {

constexpr Color cream{240, 232, 200};
constexpr Color dim{160, 168, 160};
constexpr Color ink{20, 22, 24};
constexpr Color ready{130, 220, 120};
constexpr Color alert{240, 110, 90};

} // namespace

void rect(SDL_Renderer* renderer, float x, float y, float w, float h, Color color)
{
    const SDL_FRect box{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
    SDL_RenderFillRect(renderer, &box);
}

void shade(SDL_Renderer* renderer, float x, float y, float w, float h, Color color, std::uint8_t alpha)
{
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_FRect box{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, alpha);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void text(SDL_Renderer* renderer, float x, float y, const std::string& message, Color color)
{
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
    SDL_RenderDebugText(renderer, x, y, message.c_str());
}

void centeredText(SDL_Renderer* renderer, float screenWidth, float y, const std::string& message, Color color)
{
    text(renderer, (screenWidth - 8.0F * static_cast<float>(message.size())) * 0.5F, y, message, color);
}

void notice(SDL_Renderer* renderer, float screenWidth, float y, const std::string& message, Color box, Color color)
{
    const float boxWidth = 8.0F * static_cast<float>(message.size()) + 28.0F;
    rect(renderer, (screenWidth - boxWidth) * 0.5F, y, boxWidth, 22.0F, box);
    centeredText(renderer, screenWidth, y + 7.0F, message, color);
}

void poolCounter(SDL_Renderer* renderer, float x, float y, const World& world, std::size_t gameplayReserve,
                 std::uint64_t skipped)
{
    const std::size_t capacity = world.objectCapacity();
    if (capacity == 0) {
        shade(renderer, x - 6.0F, y - 6.0F, 252.0F, 22.0F, ink, 200);
        text(renderer, x, y, "HEAP WORLD: NO POOL", dim);
        return;
    }
    shade(renderer, x - 6.0F, y - 6.0F, 252.0F, 66.0F, ink, 200);
    char line[64];
    std::snprintf(line, sizeof line, "POOL %zu / %zu OBJECTS", world.objects().size(), capacity);
    text(renderer, x, y, line, cream);
    std::snprintf(line, sizeof line, "GAMEPLAY RESERVE %zu", gameplayReserve);
    text(renderer, x, y + 12.0F, line, dim);
    std::snprintf(line, sizeof line, "COSMETICS SKIPPED %llu", static_cast<unsigned long long>(skipped));
    text(renderer, x, y + 24.0F, line, skipped > 0 ? alert : dim);

    const float barWidth = 240.0F;
    const float used = static_cast<float>(world.objects().size()) / static_cast<float>(capacity);
    rect(renderer, x, y + 38.0F, barWidth, 8.0F, {50, 60, 62});
    rect(renderer, x, y + 38.0F, barWidth * std::min(used, 1.0F), 8.0F, used > 0.9F ? alert : ready);
    // Cosmetics stop at this mark; the slots beyond it are kept for gameplay.
    const float reserveMark = static_cast<float>(capacity - gameplayReserve) / static_cast<float>(capacity);
    rect(renderer, x + barWidth * reserveMark, y + 35.0F, 2.0F, 14.0F, cream);
}

} // namespace Arcade
