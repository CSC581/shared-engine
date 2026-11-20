#pragma once

#include "EventManager.hpp"
#include "FrameTime.hpp"
#include "Game.hpp"
#include "PirateCannonsMatch.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

class Engine;

// The window side of Pirate Cannons: input through InputEvents and the
// chain-shot chord, pause, the pool counter and all drawing. The rules and
// the computer opponent live in PirateCannons::Match.
class PirateCannonsGame final : public Game {
public:
    PirateCannonsGame(Engine& engine, std::uint32_t seed);
    ~PirateCannonsGame() override;

    PirateCannonsGame(const PirateCannonsGame&) = delete;
    PirateCannonsGame& operator=(const PirateCannonsGame&) = delete;

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void update(const FrameTime& time, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    void onInput(const Event& event);
    bool held(int scancode) const;
    void drawScenery(SDL_Renderer* renderer) const;
    void drawShip(SDL_Renderer* renderer, PirateCannons::Side side) const;
    void drawSea(SDL_Renderer* renderer) const;
    void drawGulls(SDL_Renderer* renderer) const;
    void drawAim(SDL_Renderer* renderer) const;
    void drawHud(SDL_Renderer* renderer) const;
    void drawPool(SDL_Renderer* renderer) const;

    Engine& engine_;
    PirateCannons::Match match_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
    std::unordered_set<int> heldKeys_;
    bool paused_ = false;
    bool showPool_ = false;
    // Real time, for waves and wings that keep moving while paused.
    double realSeconds_ = 0.0;
    std::int64_t nextLoopStartNs_ = 0;
};
