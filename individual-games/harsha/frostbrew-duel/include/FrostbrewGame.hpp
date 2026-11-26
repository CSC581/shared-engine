#pragma once

#include "FrostbrewArena.hpp"
#include "Game.hpp"

#include <SDL3/SDL_scancode.h>

#include <array>
#include <vector>

class Engine;
struct SDL_Texture;

namespace Frostbrew {
class DuelGame final : public Game {
public:
    explicit DuelGame(Engine& engine);
    ~DuelGame() override;
    void onEvent(const SDL_Event& event) override;
    void handleInput(Engine& engine) override;
    void update(float seconds, Engine& engine) override;
    void update(const FrameTime& time, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;
    Arena& arena();
    bool focused() const;

private:
    void input(const Event& event);
    void loadSprites();
    void releaseSprites();
    void penguin(SDL_Renderer* renderer, std::size_t player) const;
    void hud(SDL_Renderer* renderer) const;
    void bottle(SDL_Renderer* renderer, float x, float y, Color color, float scale) const;

    Engine& engine_;
    Arena arena_;
    EventManager::SubscriptionId inputSubscription_ = 0;
    std::array<bool, SDL_SCANCODE_COUNT> held_{};
    std::array<Ingredient, 2> ingredients_{{Ingredient::Ember, Ingredient::Ember}};
    std::size_t slot_ = 0;
    bool focus_ = false;
    bool book_ = false;
    bool fire_ = false;
    bool rejected_ = false;
    std::array<SDL_Texture*, 3> sprites_{};
    std::array<int, 3> frameCounts_{};
    std::array<int, 2> previousHealth_{{100, 100}};
    std::array<double, 2> hurt_{};
    std::array<double, 2> throwing_{};
    double animation_ = 0;
    std::vector<Point> prediction_;
};
} // namespace Frostbrew
