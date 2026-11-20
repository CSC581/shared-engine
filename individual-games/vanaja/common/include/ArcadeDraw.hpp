#pragma once

#include "Color.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

struct SDL_Renderer;
class World;

// Drawing helpers Garden Defense and Pirate Cannons share: filled boxes,
// debug text, boxed notices and the F3 pool counter.
namespace Arcade {

void rect(SDL_Renderer* renderer, float x, float y, float w, float h, Color color);

// Translucent fill; restores the renderer's opaque drawing afterwards.
void shade(SDL_Renderer* renderer, float x, float y, float w, float h, Color color, std::uint8_t alpha);

void text(SDL_Renderer* renderer, float x, float y, const std::string& message, Color color);

// Debug text is 8 px per character.
void centeredText(SDL_Renderer* renderer, float screenWidth, float y, const std::string& message, Color color);

// A boxed message across the middle of the screen.
void notice(SDL_Renderer* renderer, float screenWidth, float y, const std::string& message, Color box, Color color);

// How full the world's fixed slots are, the slots kept for gameplay (marked
// on the bar), and how many cosmetic objects were skipped near the limit.
void poolCounter(SDL_Renderer* renderer, float x, float y, const World& world, std::size_t gameplayReserve,
                 std::uint64_t skipped);

} // namespace Arcade
