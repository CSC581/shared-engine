// Apex Ascent: climb a tower of platforms one charged jump at a time.
//
// Hold SPACE to charge a jump, aim with A/D while charging, release to leap.
// Momentum from a jump is not overwritten by input until you land again, so
// once you commit to a jump you ride it out.
//
// This is the individual game -- everything here is this game's own choice and
// the engine has no idea any of it exists. Self-contained: class, game logic
// and main() in one file, following the pattern of the other games/ examples.
#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>  // do we need this?

#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <vector>

namespace {

// An individual game built on the shared engine.
//
// The world is several screens tall ("rooms"), stacked with room 0 at the
// bottom and room (roomCount - 1) at the top. The player climbs upward
// (decreasing y); a vertical camera follows them so only one room's worth of
// the world is visible at a time.
class ApexAscent : public Game {
public:
    explicit ApexAscent(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 2200.0F;

    // Jump charging: hold SPACE longer for a bigger leap, aim with A/D.
    static constexpr float baseJumpSpeed = 480.0F;
    static constexpr float maxChargePower = 1500.0F;
    static constexpr float chargeSpeed = 1100.0F;
    // How much of the jump's power goes sideways when a direction is held.
    static constexpr float horizontalJumpRatio = 0.55F;

    static constexpr float playerWidth = 50.0F;
    static constexpr float playerHeight = 70.0F;

    static constexpr int roomCount = 6;

    void buildLevel();
    void handleCollisions();
    void updateCamera(float deltaTime);

    Entity player_;
    std::vector<Entity> platforms_;

    float viewWidth_;
    float viewHeight_;
    float worldWidth_;
    float worldHeight_;

    float startX_;
    float startY_;

    // Vertical scroll offset: world y minus camera_ is screen y.
    float camera_ = 0.0F;

    float chargePower_ = 0.0F;
    float aimDirection_ = 0.0F;

    bool isCharging_ = false;
    bool isOnGround_ = false;

    int highestRoomReached_ = 0;
};

ApexAscent::ApexAscent(const Engine& engine)
    : player_(0.0F, 0.0F, playerWidth, playerHeight),
      viewWidth_(static_cast<float>(engine.getWidth())),
      viewHeight_(static_cast<float>(engine.getHeight())),
      worldWidth_(viewWidth_),
      worldHeight_(viewHeight_ * static_cast<float>(roomCount)),
      startX_(worldWidth_ * 0.5F - playerWidth * 0.5F),
      startY_(worldHeight_ - 140.0F - playerHeight)
{
    player_.setPosition(startX_, startY_);

    buildLevel();

    // This game's choice of gravity, not a value baked into the engine.
    Physics::setGravity(gravity);

    camera_ = worldHeight_ - viewHeight_;

    std::cout << "Apex Ascent: A/D to aim, hold Space to charge a jump, "
                 "release to leap. F1 to toggle scaling, Esc to quit.\n";
}

// Static level geometry: ground, side walls to keep the climb in bounds, and
// a simple staircase of platforms up through every room. Replace this with
// your own level design -- it exists to give you something to jump on.
void ApexAscent::buildLevel()
{
    constexpr float groundThickness = 140.0F;
    constexpr float wallThickness = 40.0F;

    // Ground, at the very bottom of the world.
    platforms_.emplace_back(0.0F, worldHeight_ - groundThickness, worldWidth_, groundThickness);

    // Side walls, just outside the playable width, for the whole climb.
    platforms_.emplace_back(-wallThickness, 0.0F, wallThickness, worldHeight_);
    platforms_.emplace_back(worldWidth_, 0.0F, wallThickness, worldHeight_);

    // A hand-placed first room, easy jumps to learn the charge mechanic.
    const float room0Top = worldHeight_ - viewHeight_;
    platforms_.emplace_back(160.0F, room0Top + 620.0F, 220.0F, 40.0F);
    platforms_.emplace_back(560.0F, room0Top + 460.0F, 220.0F, 40.0F);
    platforms_.emplace_back(220.0F, room0Top + 280.0F, 220.0F, 40.0F);

    // TODO: replace this generated staircase with hand-designed rooms once
    // the core feel (charge, jump, land) works the way you want.
    for (int room = 1; room < roomCount; ++room) {
        const float roomTop = worldHeight_ - viewHeight_ * static_cast<float>(room + 1);
        const bool startLeft = room % 2 == 0;

        const float leftX = 120.0F;
        const float rightX = worldWidth_ - 120.0F - 220.0F;

        platforms_.emplace_back(startLeft ? leftX : rightX, roomTop + 700.0F, 220.0F, 40.0F);
        platforms_.emplace_back(startLeft ? rightX : leftX, roomTop + 460.0F, 220.0F, 40.0F);
        platforms_.emplace_back(worldWidth_ * 0.5F - 110.0F, roomTop + 220.0F, 220.0F, 40.0F);
    }
}

void ApexAscent::handleInput(Engine& engine)
{
    // Aim while charging. Once airborne, momentum is locked in -- no air
    // control -- so this only matters while isOnGround_ is true.
    if (isOnGround_) {
        float aim = 0.0F;
        if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
            aim -= 1.0F;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
            aim += 1.0F;
        }
        aimDirection_ = aim;

        if (Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            isCharging_ = true;
        }
    }

    if (Input::isKeyJustReleased(SDL_SCANCODE_SPACE) && isCharging_) {
        isCharging_ = false;

        const float power = baseJumpSpeed + chargePower_;
        player_.setVelocity(aimDirection_ * power * horizontalJumpRatio, -power);

        chargePower_ = 0.0F;
        isOnGround_ = false;
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
    }
}

void ApexAscent::update(float deltaTime, Engine& engine)
{
    (void)engine;

    if (isCharging_) {
        chargePower_ += chargeSpeed * deltaTime;
        if (chargePower_ > maxChargePower) {
            chargePower_ = maxChargePower;
        }
    }

    if (!isOnGround_) {
        Physics::applyGravity(player_, deltaTime);
    }
    player_.update(deltaTime);

    handleCollisions();
    updateCamera(deltaTime);

    const int room = static_cast<int>((worldHeight_ - player_.getY()) / viewHeight_);
    if (room > highestRoomReached_) {
        highestRoomReached_ = room;
    }
}

// Everything the game decides to do once the engine reports an overlap.
void ApexAscent::handleCollisions()
{
    isOnGround_ = false;

    for (const Entity& platform : platforms_) {
        float pushX = 0.0F;
        float pushY = 0.0F;

        if (!Collision::getSeparation(player_, platform, pushX, pushY)) {
            continue;
        }

        if (pushY < 0.0F) {
            // Landed on top of something: footing regained, jump consumed.
            isOnGround_ = true;
        } else if (pushY > 0.0F) {
            // Bonked head-first into a platform from below.
            // TODO: add a knockback/"bonk" reaction here if you want one --
            // resolve() below just stops the upward motion.
        }

        Collision::resolve(player_, platform);
    }
}

// Smoothly scrolls the camera to follow the player vertically. Rooms are not
// snapped to discrete screens yet -- this just keeps the player roughly
// centred -- so treat it as a starting point for your own camera behaviour.
void ApexAscent::updateCamera(float deltaTime)
{
    const float target = player_.getY() + playerHeight * 0.5F - viewHeight_ * 0.5F;
    camera_ += (target - camera_) * (1.0F - std::pow(0.001F, deltaTime));

    if (camera_ < 0.0F) {
        camera_ = 0.0F;
    }
    if (camera_ > worldHeight_ - viewHeight_) {
        camera_ = worldHeight_ - viewHeight_;
    }
}

void ApexAscent::render(SDL_Renderer* renderer) const
{
    // Everything below draws in world space, shifted by the camera.
    const auto drawRect = [this, renderer](const Rect& bounds, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        const SDL_FRect rect{bounds.x, bounds.y - camera_, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    };

    for (const Entity& platform : platforms_) {
        drawRect(platform.getBounds(), 90, 100, 130);
    }

    drawRect(player_.getBounds(), 240, 240, 255);

    // Charge meter, drawn in screen space rather than world space.
    const float meterWidth = 220.0F;
    const float filled = meterWidth * chargePower_ / maxChargePower;
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    const SDL_FRect meterBack{20.0F, viewHeight_ - 40.0F, meterWidth, 16.0F};
    SDL_RenderFillRect(renderer, &meterBack);

    SDL_SetRenderDrawColor(renderer, 250, 210, 80, 255);
    const SDL_FRect meterFill{20.0F, viewHeight_ - 40.0F, filled, 16.0F};
    SDL_RenderFillRect(renderer, &meterFill);

    char hud[128];
    std::snprintf(hud, sizeof(hud), "Room: %d / %d   (highest: %d)",
                  static_cast<int>((worldHeight_ - player_.getY()) / viewHeight_) + 1,
                  roomCount, highestRoomReached_ + 1);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20.0F, 20.0F, hud);
}

} // namespace

int main()
{
    try {
        Engine engine("Apex Ascent", 900, 1200);
        engine.setClearColor(18, 20, 32);

        ApexAscent game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
