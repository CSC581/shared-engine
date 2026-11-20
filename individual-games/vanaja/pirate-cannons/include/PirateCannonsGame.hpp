#pragma once

#include "ArcadeParts.hpp"
#include "EventManager.hpp"
#include "FrameTime.hpp"
#include "Game.hpp"
#include "PirateCannonsMatch.hpp"

#include <cstdint>
#include <unordered_map>
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
    // Real seconds when the key last went down; 0 if never.
    double pressedAt(int scancode) const;
    void drawScenery(SDL_Renderer* renderer) const;
    void drawShip(SDL_Renderer* renderer, PirateCannons::Side side) const;
    void drawSea(SDL_Renderer* renderer) const;
    void drawGulls(SDL_Renderer* renderer) const;
    void drawAim(SDL_Renderer* renderer) const;
    void drawHud(SDL_Renderer* renderer) const;

    Engine& engine_;
    PirateCannons::Match match_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
    std::unordered_set<int> heldKeys_;
    std::unordered_map<int, double> pressedAt_;
    // Space was pressed this frame. It fires a plain shot in update(), after
    // the frame's chord event has had the chance to fire a chain shot.
    bool spaceQueued_ = false;
    bool paused_ = false;
    bool showPool_ = false;
    // Real time, for waves and wings that keep moving while paused.
    double realSeconds_ = 0.0;
    Arcade::FramePacer pacer_{PirateCannons::loopHz};
};
