#pragma once

#include "ArcadeParts.hpp"
#include "EventManager.hpp"
#include "FrameTime.hpp"
#include "Game.hpp"
#include "GardenDefenseMatch.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Engine;

// The window side of Garden Defense: input through InputEvents and the
// umbrella chord, pause, the pool counter and all drawing. The rules live in
// GardenDefense::Match.
class GardenDefenseGame final : public Game {
public:
    GardenDefenseGame(Engine& engine, std::uint32_t seed);
    ~GardenDefenseGame() override;

    GardenDefenseGame(const GardenDefenseGame&) = delete;
    GardenDefenseGame& operator=(const GardenDefenseGame&) = delete;

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void update(const FrameTime& time, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    void onInput(const Event& event);
    bool held(int scancode) const;
    // Real seconds when the key last went down; 0 if never.
    double pressedAt(int scancode) const;
    void drawGarden(SDL_Renderer* renderer) const;
    void drawPests(SDL_Renderer* renderer) const;
    void drawCan(SDL_Renderer* renderer) const;
    void drawHud(SDL_Renderer* renderer) const;

    Engine& engine_;
    GardenDefense::Match match_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
    std::unordered_set<int> heldKeys_;
    std::unordered_map<int, double> pressedAt_;
    bool paused_ = false;
    bool showPool_ = false;
    // Real time, for blinking that keeps going while paused.
    double realSeconds_ = 0.0;
    Arcade::FramePacer pacer_{GardenDefense::loopHz};
};
