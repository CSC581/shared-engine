#pragma once

#include "Game.hpp"
#include "NeonArena.hpp"
#include "EventManager.hpp"
#include <SDL3/SDL_scancode.h>
#include <array>
#include <memory>

class Engine;
namespace Neon {
class SiegeGame final : public Game {
public:
    explicit SiegeGame(Engine& engine);
    ~SiegeGame() override;
    void onEvent(const SDL_Event& event) override;
    void handleInput(Engine& engine) override;
    void update(float seconds, Engine& engine) override;
    void update(const FrameTime& time, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;
    Arena& arena();
    void setAutoplay(bool enabled);
private:
    void input(const Event& event);
    void clearInput();
    Engine& engine_;
    std::unique_ptr<Arena> arena_;
    EventManager::SubscriptionId subscription_ = 0;
    std::array<bool, SDL_SCANCODE_COUNT> held_{};
    bool shield_ = false, autoplay_ = false;
};
} // namespace Neon
