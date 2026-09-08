#pragma once

#include "Entity.hpp"
#include "Game.hpp"

#include <array>
#include <vector>

struct SDL_Renderer;
class Engine;

class StarfallSalvageGame final : public Game {
public:
    StarfallSalvageGame();

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    enum class MissionState {
        Playing,
        Complete,
        Failed,
    };

    void buildLevel();
    void resetPlayer();
    void restartMission();
    void updatePlayer(float deltaTime);
    void updateCargo(float deltaTime);
    void updateDrones(float deltaTime);
    void collectCargo();
    void handleDroneCollision();

    Entity player_{0.0F, 0.0F, 34.0F, 42.0F};
    std::vector<Entity> platforms_;
    std::vector<Entity> drones_;
    std::vector<Entity> cargo_;
    std::array<bool, 4> collected_{{false, false, false, false}};

    float moveX_ = 0.0F;
    int lives_ = 3;
    int recovered_ = 0;
    int jumpsUsed_ = 0;
    bool grounded_ = false;
    MissionState state_ = MissionState::Playing;
};
