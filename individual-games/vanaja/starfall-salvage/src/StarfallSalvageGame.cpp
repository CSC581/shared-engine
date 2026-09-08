#include "StarfallSalvageGame.hpp"

#include "Collision.hpp"
#include "Engine.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>

namespace {
constexpr float playerSpeed = 260.0F;
constexpr float droneSpeed = 115.0F;
constexpr float jumpSpeed = 500.0F;
constexpr int maximumJumps = 2;

struct Star {
    float x;
    float y;
    float size;
    Uint8 brightness;
};

struct DronePatrol {
    float minimumX;
    float maximumX;
};

struct DroneSpec {
    Rect spawn;
    DronePatrol patrol;
    float initialVelocityX;
};

constexpr std::array<Star, 24> stars{{
    {55.0F, 65.0F, 2.0F, 112}, {112.0F, 132.0F, 3.0F, 185}, {175.0F, 70.0F, 2.0F, 145},
    {242.0F, 205.0F, 2.0F, 125}, {315.0F, 80.0F, 3.0F, 180}, {392.0F, 172.0F, 2.0F, 135},
    {451.0F, 55.0F, 2.0F, 165}, {532.0F, 145.0F, 3.0F, 205}, {606.0F, 82.0F, 2.0F, 135},
    {673.0F, 122.0F, 2.0F, 185}, {745.0F, 58.0F, 3.0F, 130}, {835.0F, 113.0F, 2.0F, 190},
    {905.0F, 74.0F, 2.0F, 145}, {94.0F, 285.0F, 3.0F, 175}, {160.0F, 455.0F, 2.0F, 145},
    {282.0F, 445.0F, 2.0F, 185}, {402.0F, 390.0F, 3.0F, 120}, {570.0F, 450.0F, 2.0F, 205},
    {655.0F, 375.0F, 2.0F, 135}, {805.0F, 465.0F, 3.0F, 175}, {885.0F, 315.0F, 2.0F, 145},
    {55.0F, 380.0F, 2.0F, 175}, {910.0F, 205.0F, 3.0F, 135}, {360.0F, 490.0F, 2.0F, 145},
}};

constexpr Rect playerSpawn{120.0F, 388.0F, 34.0F, 42.0F};

constexpr std::array<Rect, 7> platformLayouts{{
    {0.0F, 500.0F, 960.0F, 40.0F},
    {0.0F, 0.0F, 35.0F, 540.0F},
    {925.0F, 0.0F, 35.0F, 540.0F},
    // A clear, forgiving route: start left, climb to the right, then return
    // to the final cargo bay. Each step is well inside the jump height.
    {60.0F, 430.0F, 230.0F, 28.0F},
    {330.0F, 350.0F, 250.0F, 28.0F},
    {620.0F, 270.0F, 250.0F, 28.0F},
    {350.0F, 190.0F, 240.0F, 28.0F},
}};

constexpr std::array<Rect, 4> cargoSpawns{{
    {250.0F, 405.0F, 25.0F, 25.0F},
    {545.0F, 325.0F, 25.0F, 25.0F},
    {830.0F, 245.0F, 25.0F, 25.0F},
    {535.0F, 165.0F, 25.0F, 25.0F},
}};

constexpr std::array<DroneSpec, 2> droneSpecs{{
    // The drones hover above separate patrol lanes rather than circling a
    // cargo crate. That leaves a clear dodge route to each pickup.
    {{350.0F, 314.0F, 46.0F, 26.0F}, {350.0F, 490.0F}, droneSpeed},
    {{630.0F, 234.0F, 46.0F, 26.0F}, {630.0F, 775.0F}, -droneSpeed},
}};

void fillRect(SDL_Renderer* renderer, float x, float y, float width, float height, Uint8 red, Uint8 green, Uint8 blue)
{
    const SDL_FRect rectangle{x, y, width, height};
    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
    SDL_RenderFillRect(renderer, &rectangle);
}

void drawPlatform(SDL_Renderer* renderer, const Entity& platform)
{
    const Rect bounds = platform.getBounds();
    fillRect(renderer, bounds.x, bounds.y, bounds.width, bounds.height, 39, 56, 87);
    fillRect(renderer, bounds.x, bounds.y, bounds.width, 4.0F, 122, 163, 204);
    fillRect(renderer, bounds.x + 4.0F, bounds.y + 5.0F, bounds.width - 8.0F, 4.0F, 68, 91, 128);
    fillRect(renderer, bounds.x + 6.0F, bounds.y + bounds.height - 5.0F, bounds.width - 12.0F, 2.0F, 24, 37, 62);

    if (bounds.width > bounds.height) {
        fillRect(renderer, bounds.x + 12.0F, bounds.y + 15.0F, 5.0F, 5.0F, 101, 146, 184);
        fillRect(renderer, bounds.x + bounds.width - 17.0F, bounds.y + 15.0F, 5.0F, 5.0F, 101, 146, 184);
    }
}

void drawStationSupport(SDL_Renderer* renderer, float x, float ceilingY, float deckY)
{
    constexpr float supportWidth = 16.0F;
    const float height = deckY - ceilingY;

    fillRect(renderer, x, ceilingY, supportWidth, height, 31, 48, 78);
    fillRect(renderer, x + 3.0F, ceilingY, 4.0F, height, 72, 105, 145);
    fillRect(renderer, x - 7.0F, deckY - 6.0F, supportWidth + 14.0F, 6.0F, 63, 91, 126);
    fillRect(renderer, x - 3.0F, ceilingY, supportWidth + 6.0F, 5.0F, 93, 132, 172);
}

void drawAstronaut(SDL_Renderer* renderer, const Entity& astronaut)
{
    const Rect bounds = astronaut.getBounds();
    fillRect(renderer, bounds.x - 6.0F, bounds.y + 12.0F, 7.0F, 23.0F, 92, 139, 169);
    fillRect(renderer, bounds.x + 3.0F, bounds.y + 13.0F, 28.0F, 25.0F, 183, 225, 238);
    fillRect(renderer, bounds.x + 5.0F, bounds.y + 2.0F, 24.0F, 18.0F, 211, 240, 247);
    fillRect(renderer, bounds.x + 8.0F, bounds.y + 5.0F, 18.0F, 10.0F, 38, 109, 143);
    fillRect(renderer, bounds.x + 4.0F, bounds.y + 38.0F, 10.0F, 4.0F, 102, 156, 181);
    fillRect(renderer, bounds.x + 20.0F, bounds.y + 38.0F, 10.0F, 4.0F, 102, 156, 181);
}

void drawDrone(SDL_Renderer* renderer, const Entity& drone)
{
    const Rect bounds = drone.getBounds();
    fillRect(renderer, bounds.x + 2.0F, bounds.y + 7.0F, bounds.width - 4.0F, 15.0F, 160, 57, 81);
    fillRect(renderer, bounds.x + 9.0F, bounds.y + 3.0F, bounds.width - 18.0F, 21.0F, 231, 81, 105);
    fillRect(renderer, bounds.x + 18.0F, bounds.y + 8.0F, 10.0F, 7.0F, 255, 220, 112);
    fillRect(renderer, bounds.x + 3.0F, bounds.y + 24.0F, 8.0F, 3.0F, 101, 195, 255);
    fillRect(renderer, bounds.x + 35.0F, bounds.y + 24.0F, 8.0F, 3.0F, 101, 195, 255);
}

void drawCargo(SDL_Renderer* renderer, const Entity& cargo)
{
    const Rect bounds = cargo.getBounds();
    fillRect(renderer, bounds.x, bounds.y, bounds.width, bounds.height, 213, 112, 47);
    fillRect(renderer, bounds.x + 3.0F, bounds.y + 3.0F, bounds.width - 6.0F, bounds.height - 6.0F, 255, 184, 76);
    fillRect(renderer, bounds.x + 10.0F, bounds.y + 3.0F, 4.0F, bounds.height - 6.0F, 148, 70, 39);
    fillRect(renderer, bounds.x + 3.0F, bounds.y + 10.0F, bounds.width - 6.0F, 4.0F, 148, 70, 39);
}
} // namespace

StarfallSalvageGame::StarfallSalvageGame()
{
    Physics::setGravity(900.0F);
    buildLevel();
    resetPlayer();
}

void StarfallSalvageGame::buildLevel()
{
    platforms_.clear();
    platforms_.reserve(platformLayouts.size());
    for (const Rect& layout : platformLayouts) {
        platforms_.emplace_back(layout.x, layout.y, layout.width, layout.height);
    }

    cargo_.clear();
    cargo_.reserve(cargoSpawns.size());
    for (const Rect& spawn : cargoSpawns) {
        cargo_.emplace_back(spawn.x, spawn.y, spawn.width, spawn.height);
    }

    drones_.clear();
    drones_.reserve(droneSpecs.size());
    for (const DroneSpec& spec : droneSpecs) {
        drones_.emplace_back(spec.spawn.x, spec.spawn.y, spec.spawn.width, spec.spawn.height);
        drones_.back().setVelocityX(spec.initialVelocityX);
    }

    collected_.fill(false);
}

void StarfallSalvageGame::handleInput(Engine& engine)
{
    moveX_ = 0.0F;
    if (state_ == MissionState::Playing &&
        (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT))) {
        moveX_ -= 1.0F;
    }
    if (state_ == MissionState::Playing &&
        (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT))) {
        moveX_ += 1.0F;
    }
    if ((Input::isKeyJustPressed(SDL_SCANCODE_SPACE) || Input::isKeyJustPressed(SDL_SCANCODE_W) ||
         Input::isKeyJustPressed(SDL_SCANCODE_UP)) && jumpsUsed_ < maximumJumps &&
        state_ == MissionState::Playing) {
        player_.setVelocityY(-jumpSpeed);
        ++jumpsUsed_;
        grounded_ = false;
    }

    if (state_ != MissionState::Playing && Input::isKeyJustPressed(SDL_SCANCODE_R)) {
        restartMission();
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
    }
}

void StarfallSalvageGame::update(float deltaTime, Engine&)
{
    if (state_ != MissionState::Playing) {
        return;
    }

    updatePlayer(deltaTime);
    updateCargo(deltaTime);
    updateDrones(deltaTime);
    collectCargo();

    for (const Entity& drone : drones_) {
        if (player_.collidesWith(drone)) {
            handleDroneCollision();
            break;
        }
    }
}

void StarfallSalvageGame::render(SDL_Renderer* renderer) const
{
    // A recessed starfield keeps the station interior from reading as a flat
    // blue box while the structural frame stays behind the playable decks.
    fillRect(renderer, 35.0F, 50.0F, 890.0F, 450.0F, 14, 23, 43);
    fillRect(renderer, 42.0F, 57.0F, 876.0F, 436.0F, 11, 19, 36);

    for (const Star& star : stars) {
        fillRect(renderer, star.x, star.y, star.size, star.size, star.brightness, star.brightness, 255);
    }

    // The ceiling truss and supports line up with their decks, so the level
    // reads as one station bay instead of a group of floating rectangles.
    fillRect(renderer, 42.0F, 88.0F, 876.0F, 16.0F, 30, 47, 77);
    fillRect(renderer, 42.0F, 88.0F, 876.0F, 3.0F, 99, 141, 184);
    fillRect(renderer, 55.0F, 104.0F, 850.0F, 3.0F, 19, 31, 55);
    drawStationSupport(renderer, 175.0F, 104.0F, 430.0F);
    drawStationSupport(renderer, 455.0F, 104.0F, 350.0F);
    drawStationSupport(renderer, 745.0F, 104.0F, 270.0F);
    drawStationSupport(renderer, 505.0F, 104.0F, 190.0F);

    for (const Entity& platform : platforms_) {
        drawPlatform(renderer, platform);
    }

    fillRect(renderer, 58.0F, 430.0F, 50.0F, 70.0F, 37, 70, 100);
    fillRect(renderer, 64.0F, 437.0F, 38.0F, 57.0F, 74, 222, 244);
    fillRect(renderer, 70.0F, 445.0F, 26.0F, 12.0F, 155, 245, 255);

    for (std::size_t index = 0; index < cargo_.size(); ++index) {
        if (!collected_[index]) {
            drawCargo(renderer, cargo_[index]);
        }
    }

    for (const Entity& drone : drones_) {
        drawDrone(renderer, drone);
    }

    drawAstronaut(renderer, player_);

    fillRect(renderer, 18.0F, 12.0F, 924.0F, 28.0F, 9, 16, 32);
    SDL_SetRenderDrawColor(renderer, 156, 233, 255, 255);
    SDL_RenderDebugText(renderer, 28.0F, 21.0F, "STARFALL SALVAGE");
    SDL_SetRenderDrawColor(renderer, 255, 201, 89, 255);
    SDL_RenderDebugTextFormat(renderer, 390.0F, 21.0F, "CARGO %d/4", recovered_);
    SDL_SetRenderDrawColor(renderer, 255, 118, 139, 255);
    SDL_RenderDebugTextFormat(renderer, 620.0F, 21.0F, "LIVES %d", lives_);
    SDL_SetRenderDrawColor(renderer, 138, 170, 210, 255);
    SDL_RenderDebugText(renderer, 735.0F, 21.0F, "F1: SCALE");

    if (state_ != MissionState::Playing) {
        fillRect(renderer, 280.0F, 190.0F, 400.0F, 130.0F, 8, 15, 30);
        fillRect(renderer, 284.0F, 194.0F, 392.0F, 122.0F,
                 state_ == MissionState::Complete ? 35 : 110,
                 state_ == MissionState::Complete ? 108 : 35,
                 state_ == MissionState::Complete ? 88 : 54);
        SDL_SetRenderDrawColor(renderer,
                               state_ == MissionState::Complete ? 135 : 255,
                               state_ == MissionState::Complete ? 244 : 119,
                               state_ == MissionState::Complete ? 205 : 139,
                               255);
        SDL_RenderDebugText(renderer, 385.0F, 228.0F,
                            state_ == MissionState::Complete ? "MISSION COMPLETE" : "MISSION FAILED");
        SDL_SetRenderDrawColor(renderer, 230, 240, 255, 255);
        SDL_RenderDebugText(renderer, 412.0F, 270.0F, "PRESS R TO RESTART");
    }
}

void StarfallSalvageGame::resetPlayer()
{
    player_.setPosition(playerSpawn.x, playerSpawn.y);
    player_.setSize(playerSpawn.width, playerSpawn.height);
    player_.setVelocity(0.0F, 0.0F);
    jumpsUsed_ = 0;
    grounded_ = false;
}

void StarfallSalvageGame::restartMission()
{
    lives_ = 3;
    recovered_ = 0;
    state_ = MissionState::Playing;
    buildLevel();
    resetPlayer();
}

void StarfallSalvageGame::updatePlayer(float deltaTime)
{
    player_.setVelocityX(moveX_ * playerSpeed);
    Physics::applyGravity(player_, deltaTime);
    player_.update(deltaTime);

    grounded_ = false;
    for (const Entity& platform : platforms_) {
        float separationX = 0.0F;
        float separationY = 0.0F;
        if (Collision::getSeparation(player_, platform, separationX, separationY) && separationY < 0.0F) {
            grounded_ = true;
            jumpsUsed_ = 0;
        }
        Collision::resolve(player_, platform);
    }
}

void StarfallSalvageGame::updateCargo(float deltaTime)
{
    for (std::size_t index = 0; index < cargo_.size(); ++index) {
        if (collected_[index]) {
            continue;
        }
        Physics::applyGravity(cargo_[index], deltaTime);
        cargo_[index].update(deltaTime);
        for (const Entity& platform : platforms_) {
            Collision::resolve(cargo_[index], platform);
        }
    }
}

void StarfallSalvageGame::updateDrones(float deltaTime)
{
    for (std::size_t index = 0; index < drones_.size(); ++index) {
        Entity& drone = drones_[index];
        drone.update(deltaTime);
        const Rect bounds = drone.getBounds();
        const DronePatrol patrol = droneSpecs[index].patrol;

        if (bounds.x < patrol.minimumX || bounds.x > patrol.maximumX) {
            const float boundary = bounds.x < patrol.minimumX ? patrol.minimumX : patrol.maximumX;
            drone.setPosition(boundary, bounds.y);
            drone.setVelocityX(boundary == patrol.minimumX ? droneSpeed : -droneSpeed);
        }
    }
}

void StarfallSalvageGame::collectCargo()
{
    bool allCollected = true;
    for (std::size_t index = 0; index < cargo_.size(); ++index) {
        if (!collected_[index] && player_.collidesWith(cargo_[index])) {
            collected_[index] = true;
            ++recovered_;
            std::printf("Cargo recovered: %d/4\n", recovered_);
        }
        allCollected = allCollected && collected_[index];
    }

    if (allCollected) {
        state_ = MissionState::Complete;
        std::printf("Mission complete. All cargo recovered.\n");
    }
}

void StarfallSalvageGame::handleDroneCollision()
{
    --lives_;
    if (lives_ == 0) {
        state_ = MissionState::Failed;
        player_.setVelocity(0.0F, 0.0F);
        std::printf("Mission failed. Press R to restart.\n");
        return;
    }

    std::printf("Repair drone collision. Lives remaining: %d\n", lives_);
    resetPlayer();
}
