#include "NeonGame.hpp"
#include "Engine.hpp"
#include <SDL3/SDL.h>
#include <cmath>

namespace Neon {
namespace {
void text(SDL_Renderer* r, float x, float y, const char* value, Uint8 red = 190, Uint8 green = 205, Uint8 blue = 235)
{
    SDL_SetRenderDrawColor(r, red, green, blue, 255); SDL_RenderDebugText(r, x, y, value);
}
}
void SiegeGame::render(SDL_Renderer* renderer) const
{
    SDL_SetRenderDrawColor(renderer, 8, 12, 30, 255);
    SDL_FRect background{0, 0, Arena::Width, Arena::Height}; SDL_RenderFillRect(renderer, &background);
    SDL_SetRenderDrawColor(renderer, 28, 40, 64, 255);
    for (int x = 0; x < Arena::Width; x += 48) SDL_RenderLine(renderer, float(x), 80, float(x), 620);
    for (int y = 92; y < 620; y += 48) SDL_RenderLine(renderer, 0, float(y), Arena::Width, float(y));
    for (int i = 0; i < 90; ++i) {
        const float x = float((i * 137 + 23) % Arena::Width), y = float((i * 83 + 117) % 510 + 85);
        SDL_SetRenderDrawColor(renderer, 80, 100, Uint8(140 + i % 70), 255);
        SDL_RenderPoint(renderer, x, y);
    }
    renderWorld(renderer, arena_->storage().world());
    for (std::size_t i = 0; i < arena_->storage().slots().size(); ++i) {
        const auto* slot = arena_->storage().get(arena_->storage().handle(i));
        if (!slot || slot->kind == Kind::Guide || slot->kind == Kind::Particle) continue;
        const auto& b = *slot->body;
        if (slot->kind == Kind::Player || slot->kind == Kind::Enemy || slot->kind == Kind::Boss) {
            SDL_SetRenderDrawColor(renderer, 12, 18, 38, 255);
            SDL_FRect cockpit{b.x + b.width * 0.3F, b.y + 4, b.width * 0.4F, b.height * 0.4F};
            SDL_RenderFillRect(renderer, &cockpit);
            SDL_SetRenderDrawColor(renderer, 210, 245, 255, 255);
            SDL_RenderLine(renderer, b.x, b.y + b.height, b.x + b.width / 2, b.y - 6);
            SDL_RenderLine(renderer, b.x + b.width / 2, b.y - 6, b.x + b.width, b.y + b.height);
            if (slot->kind == Kind::Boss) {
                SDL_SetRenderDrawColor(renderer, 255, 180, 70, 255);
                SDL_FRect bar{b.x, b.y - 16, b.width * slot->health / 40, 4}; SDL_RenderFillRect(renderer, &bar);
            }
        }
    }
    const auto* player = arena_->storage().get(arena_->player());
    if (arena_->rules().protectedNow() && player) {
        SDL_SetRenderDrawColor(renderer, 90, 240, 255, 255);
        const auto& b = *player->body;
        SDL_FRect shield{b.x - 9, b.y - 10, b.width + 18, b.height + 20};
        SDL_RenderRect(renderer, &shield);
    }
    SDL_SetRenderDrawColor(renderer, 50, 115, 140, 255);
    SDL_RenderLine(renderer, 16, 620, 944, 620);
    text(renderer, 24, 24, "N E O N   S I E G E", 90, 240, 255);
    SDL_RenderDebugTextFormat(renderer, 24, 48, "WAVE %d / 3    LIVES %d    SCORE %d", arena_->rules().wave(), arena_->rules().lives(), arena_->rules().score());
    SDL_RenderDebugTextFormat(renderer, 560, 24, "SHIELD %s", arena_->rules().shieldCooldown() == 0 ? "READY" : "RECHARGING");
    SDL_RenderDebugTextFormat(renderer, 560, 48, "OBJECTS %zu / %zu", arena_->storage().stats().active, arena_->storage().capacity());
    text(renderer, 24, 690, "A/D or arrows MOVE   SPACE FIRE   LEFT+RIGHT SHIELD   P PAUSE   R RESTART");
    if (engine_.gameTime().isPaused() || arena_->rules().phase() != Phase::Playing) {
        SDL_SetRenderDrawColor(renderer, 15, 22, 45, 255);
        SDL_FRect panel{230, 290, 500, 110}; SDL_RenderFillRect(renderer, &panel);
        const char* title = engine_.gameTime().isPaused() ? "PAUSED - P TO RESUME" :
            (arena_->rules().phase() == Phase::Won ? "SECTOR CLEAR - YOU WIN" : "DEFENSE LOST");
        text(renderer, 350, 325, title, 120, 245, 250);
        text(renderer, 330, 360, "R TO RESTART    ESC TO QUIT");
    }
}
} // namespace Neon
