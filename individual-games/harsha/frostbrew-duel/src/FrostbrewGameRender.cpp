#include "FrostbrewGame.hpp"

#include "Components.hpp"
#include "Engine.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace Frostbrew {
namespace {
constexpr std::array<Color, 2> playerColors{{{100, 225, 225}, {255, 170, 210}}};
constexpr std::array<Color, 3> ingredientColors{{{255, 145, 80}, {170, 175, 210}, {145, 225, 145}}};
constexpr std::array<const char*, 3> ingredientNames{{"EMBER", "STONE", "GROWTH"}};
void color(SDL_Renderer* renderer, Color value)
{
    SDL_SetRenderDrawColor(renderer, value.red, value.green, value.blue, value.alpha);
}
void fill(SDL_Renderer* renderer, float x, float y, float w, float h, Color value)
{
    color(renderer, value);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}
} // namespace

void DuelGame::bottle(SDL_Renderer* renderer, float x, float y, Color value, float scale) const
{
    fill(renderer, x + 3 * scale, y, 4 * scale, 2 * scale, {180, 130, 85});
    fill(renderer, x + 3 * scale, y + 2 * scale, 4 * scale, 3 * scale, {205, 230, 240});
    fill(renderer, x, y + 5 * scale, 10 * scale, 10 * scale, {205, 230, 240});
    fill(renderer, x + scale, y + 8 * scale, 8 * scale, 6 * scale, value);
    fill(renderer, x + 2 * scale, y + 6 * scale, scale, 5 * scale, {255, 255, 255});
}
void DuelGame::penguin(SDL_Renderer* renderer, std::size_t player) const
{
    const auto* body = arena_.world().find(arena_.penguin(player))->get<Transform>();
    const std::size_t clip = arena_.match().health(player) == 0 ? 2 : hurt_[player] > 0 ? 1 : 0;
    const int frame = static_cast<int>(animation_ * (clip == 1 ? 10 : 5)) % frameCounts_[clip];
    const SDL_FRect source{static_cast<float>(frame * 64), 0, 64, 64};
    const SDL_FRect destination{body->x - 48, body->y - 32, 128, 128};
    SDL_RenderTextureRotated(renderer, sprites_[clip], &source, &destination, 0, nullptr,
                             player == 0 ? SDL_FLIP_NONE : SDL_FLIP_HORIZONTAL);
    const Color accent = playerColors[player];
    fill(renderer, body->x + 4, body->y + 12, 24, 5, accent);
    // A scarf and flask belt give the borrowed penguin an alchemist silhouette.
    fill(renderer, body->x + (player == 0 ? 2 : 23), body->y + 17, 7, 10, accent);
    bottle(renderer, body->x + (player == 0 ? 24 : -2), body->y + 17, accent, 0.75F);
    if (throwing_[player] > 0) {
        const float hand = body->x + (player == 0 ? 42.0F : -10.0F);
        color(renderer, {220, 235, 240});
        SDL_RenderLine(renderer, body->x + 16, body->y + 16, hand, body->y + 7);
        fill(renderer, hand - 2, body->y + 5, 5, 5, accent);
    }
    color(renderer, accent);
    SDL_RenderDebugTextFormat(renderer, body->x - 1, body->y - 35, "P%zu", player + 1);
    if (arena_.match().player() == player && arena_.match().phase() == Phase::Aim) {
        const float x = body->x + 16, y = body->y - 12;
        SDL_RenderLine(renderer, x - 6, y - 6, x, y);
        SDL_RenderLine(renderer, x, y, x + 6, y - 6);
        const float direction = player == 0 ? 1.0F : -1.0F;
        const float angle = arena_.angle() * 3.14159265F / 180;
        SDL_RenderLine(renderer, x, body->y + 12, x + direction * std::cos(angle) * 42,
                       body->y + 12 - std::sin(angle) * 42);
    }
}
void DuelGame::hud(SDL_Renderer* renderer) const
{
    fill(renderer, 0, 0, 1200, 150, {14, 22, 39});
    color(renderer, {220, 238, 245});
    SDL_RenderDebugText(renderer, 24, 18, "F R O S T B R E W   D U E L");
    SDL_RenderDebugText(renderer, 24, 36, "TWO ALCHEMISTS. SIX RECIPES. ONE CHANGING BATTLEFIELD.");
    for (std::size_t p = 0; p < 2; ++p) {
        const float x = p == 0 ? 24.0F : 916.0F;
        color(renderer, playerColors[p]);
        SDL_RenderDebugTextFormat(renderer, x, 60, "P%zu  %d HP", p + 1, arena_.match().health(p));
        fill(renderer, x, 78, 260, 8, {45, 55, 73});
        fill(renderer, x, 78, 2.6F * arena_.match().health(p), 8, playerColors[p]);
    }
    color(renderer, {235, 240, 250});
    SDL_RenderDebugTextFormat(renderer, 420, 60, "PLAYER %zu  /  %s", arena_.match().player() + 1,
        arena_.match().phase() == Phase::Resolving ? "POTION IN FLIGHT" : "BREW AND AIM");
    SDL_RenderDebugTextFormat(renderer, 420, 80, "ANGLE %.0f   POWER %.0f", arena_.angle(), arena_.power());
    for (std::size_t slot = 0; slot < 2; ++slot) {
        const float x = 24 + slot * 190.0F;
        const auto ingredient = static_cast<unsigned>(ingredients_[slot]);
        bottle(renderer, x, 103, ingredientColors[ingredient], 2);
        color(renderer, ingredientColors[ingredient]);
        SDL_RenderDebugTextFormat(renderer, x + 30, 112, "%s%s", slot_ == slot ? "> " : "", ingredientNames[ingredient]);
    }
    const auto& mix = recipe(ingredients_[0], ingredients_[1]);
    color(renderer, {230, 220, 180});
    SDL_RenderDebugTextFormat(renderer, 420, 112, "%s", arena_.match().discovered(arena_.match().player(), mix.potion)
        ? mix.name : "UNDISCOVERED MIXTURE - THROW TO DISCOVER");
    SDL_RenderDebugText(renderer, 916, 112, "B: RECIPE BOOK");

    fill(renderer, 0, 738, 1200, 62, {14, 22, 39});
    color(renderer, {190, 210, 225});
    SDL_RenderDebugText(renderer, 24, 752, "1 EMBER   2 STONE   3 GROWTH   TAB: CHANGE SLOT   SPACE: THROW   B: RECIPES");
    SDL_RenderDebugText(renderer, 24, 772, "UP/DOWN: ANGLE   LEFT/RIGHT: POWER   UP+DOWN: FOCUS   P: PAUSE   R: RESTART   ESC: QUIT");

    if (book_) {
        fill(renderer, 310, 185, 580, 350, {24, 36, 53});
        color(renderer, {235, 225, 190});
        SDL_RenderDebugTextFormat(renderer, 334, 208, "PLAYER %zu'S RECIPE BOOK - B TO CLOSE", arena_.match().player() + 1);
        int row = 0;
        for (int a = 0; a < 3; ++a) for (int b = a; b < 3; ++b) {
            const auto& entry = recipe(static_cast<Ingredient>(a), static_cast<Ingredient>(b));
            const bool known = arena_.match().discovered(arena_.match().player(), entry.potion);
            color(renderer, known ? Color{200, 235, 205} : Color{150, 165, 185});
            SDL_RenderDebugTextFormat(renderer, 334, 247.0F + row * 42, "%s + %s : %s",
                ingredientNames[a], ingredientNames[b], known ? entry.name : "???");
            if (known) SDL_RenderDebugText(renderer, 350, 262.0F + row * 42, entry.description);
            ++row;
        }
    }
    if (focus_) {
        color(renderer, {245, 235, 180});
        SDL_RenderDebugText(renderer, 440, 165, "FOCUS - RELEASE AN ARROW TO AIM");
    }
    if (rejected_) {
        color(renderer, {255, 160, 130});
        SDL_RenderDebugText(renderer, 420, 135, "NO FLASK SLOT AVAILABLE - TRY AGAIN");
    }
    if (engine_.gameTime().isPaused() || arena_.match().phase() == Phase::GameOver) {
        fill(renderer, 370, 300, 460, 125, {25, 37, 58});
        color(renderer, {245, 230, 190});
        if (engine_.gameTime().isPaused()) SDL_RenderDebugText(renderer, 478, 335, "PAUSED - P TO CONTINUE");
        else if (arena_.match().winner() < 0) SDL_RenderDebugText(renderer, 500, 335, "DOUBLE KNOCKOUT - DRAW");
        else SDL_RenderDebugTextFormat(renderer, 500, 335, "PLAYER %d WINS!", arena_.match().winner() + 1);
        SDL_RenderDebugText(renderer, 450, 370, "R: NEW MATCH   ESC: QUIT");
    }
}
void DuelGame::render(SDL_Renderer* renderer) const
{
    for (int i = 0; i < 65; ++i) {
        color(renderer, {75, 100, 135});
        const float x = static_cast<float>((i * 179 + 31) % 1200);
        const float y = static_cast<float>((i * 71 + 157) % 460);
        SDL_RenderPoint(renderer, x, y);
    }
    for (int x = 0; x < Arena::Width; ++x) {
        const float y = arena_.terrain().heights()[x];
        color(renderer, {53, 81, 104}); SDL_RenderLine(renderer, static_cast<float>(x), y + 12, static_cast<float>(x), 800);
        color(renderer, {150, 190, 205}); SDL_RenderLine(renderer, static_cast<float>(x), y + 4, static_cast<float>(x), y + 12);
        color(renderer, {225, 241, 242}); SDL_RenderLine(renderer, static_cast<float>(x), y, static_cast<float>(x), y + 4);
    }
    renderWorld(renderer, arena_.world());
    for (const auto& object : arena_.world().objects()) if (object->tag() == "flask" && object->isActive()) {
        const auto* body = object->get<Transform>();
        const auto* motion = object->get<Motion>();
        const float speed = std::max(1.0F, std::hypot(motion->velocityX, motion->velocityY));
        color(renderer, object->get<Renderable>()->color);
        SDL_RenderLine(renderer, body->x + 5, body->y + 5,
            body->x + 5 - motion->velocityX / speed * 18, body->y + 5 - motion->velocityY / speed * 18);
        bottle(renderer, body->x - 2.5F, body->y - 6, object->get<Renderable>()->color, 1.5F);
    }
    if (focus_) {
        color(renderer, {240, 225, 150});
        for (std::size_t i = 0; i < prediction_.size(); i += 12) {
            const SDL_FRect dot{prediction_[i].x - 1, prediction_[i].y - 1, 3, 3};
            SDL_RenderFillRect(renderer, &dot);
        }
    }
    penguin(renderer, 0); penguin(renderer, 1);
    hud(renderer);
}
} // namespace Frostbrew
