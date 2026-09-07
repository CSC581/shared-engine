// Apex Ascent: climb a tower of platforms one charged jump at a time.

#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <vector>

namespace {

class ApexAscent : public Game {
public:
    explicit ApexAscent(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 2200.0F;

    static constexpr float baseJumpSpeed = 480.0F;
    static constexpr float maxChargePower = 1500.0F;
    static constexpr float chargeSpeed = 1100.0F;
    static constexpr float horizontalJumpRatio = 0.55F;

    static constexpr float walkSpeed = 300.0F;
    // Multiplicative per-second decay of horizontal speed while grounded.
    static constexpr float groundFriction = 0.00002F;
    // Slop for the direction-aware landing check (float drift on the surface).
    static constexpr float landingTolerance = 4.0F;

    static constexpr float playerWidth = 50.0F;
    static constexpr float playerHeight = 70.0F;

    static constexpr int roomCount = 6;

    // Straight-line ping-pong path for one entry in platforms_.
    struct MovingPlatformPath {
        std::size_t platformIndex;
        float pointAX;
        float pointAY;
        float pointBX;
        float pointBY;
        float speed;
        bool movingToB = true;
    };

    void buildLevel();
    void handleCollisions();
    void updateCamera(float deltaTime);
    void updateMovingPlatforms(float deltaTime);
    bool isMovingPlatform(const Entity& platform) const;

    Entity player_;
    std::vector<Entity> platforms_;
    std::vector<MovingPlatformPath> movingPlatformPaths_;
    // Last platform stood on; used to carry the player next frame.
    Entity* groundedMovingPlatform_ = nullptr;

    float viewWidth_;
    float viewHeight_;
    float worldWidth_;
    float worldHeight_;

    float startX_;
    float startY_;

    // Vertical scroll: world Y minus camera_ is screen Y.
    float camera_ = 0.0F;

    float chargePower_ = 0.0F;
    float aimDirection_ = 0.0F;
    // Foot Y before this frame's move; used for landing detection.
    float previousPlayerBottom_ = 0.0F;

    bool isCharging_ = false;
    bool isOnGround_ = false;

    int highestRoomReached_ = 0;

    // Mirrored from the engine for the HUD scale-mode label.
    Engine::ScaleMode currentScaleMode_;
};

ApexAscent::ApexAscent(const Engine& engine)
    : player_(0.0F, 0.0F, playerWidth, playerHeight),
      viewWidth_(static_cast<float>(engine.getWidth())),
      viewHeight_(static_cast<float>(engine.getHeight())),
      worldWidth_(viewWidth_),
      worldHeight_(viewHeight_ * static_cast<float>(roomCount)),
      startX_(worldWidth_ * 0.5F - playerWidth * 0.5F),
      startY_(worldHeight_ - 140.0F - playerHeight),
      currentScaleMode_(engine.getScaleMode())
{
    player_.setPosition(startX_, startY_);
    previousPlayerBottom_ = startY_ + playerHeight;

    buildLevel();

    Physics::setGravity(gravity);

    camera_ = worldHeight_ - viewHeight_;

    std::cout << "Apex Ascent: A/D to aim, hold Space to charge a jump, "
                 "release to leap. F1 to toggle scaling, Esc to quit.\n";
}

void ApexAscent::buildLevel()
{
    constexpr float groundThickness = 140.0F;
    constexpr float wallThickness = 40.0F;
    constexpr float platformThickness = 60.0F;

    // Ground at the bottom of the world.
    platforms_.emplace_back(0.0F, worldHeight_ - groundThickness, worldWidth_, groundThickness);

    // Side walls for the whole climb.
    platforms_.emplace_back(-wallThickness, 0.0F, wallThickness, worldHeight_);
    platforms_.emplace_back(worldWidth_, 0.0F, wallThickness, worldHeight_);

    // Hand-placed first room (easy jumps to learn charging).
    const float room0Top = worldHeight_ - viewHeight_;
    platforms_.emplace_back(160.0F, room0Top + 620.0F, 220.0F, platformThickness);
    platforms_.emplace_back(560.0F, room0Top + 460.0F, 220.0F, platformThickness);
    platforms_.emplace_back(220.0F, room0Top + 280.0F, 220.0F, platformThickness);

    // Demo auto-moving platform; carries the player if they stand on it.
    {
        constexpr float movingPlatformWidth = 150.0F;
        const float movingPlatformY = room0Top + 150.0F;
        constexpr float pointAX = 300.0F;
        constexpr float pointBX = 550.0F;
        constexpr float movingPlatformSpeed = 150.0F;

        const std::size_t movingIndex = platforms_.size();
        platforms_.emplace_back(pointAX, movingPlatformY, movingPlatformWidth, platformThickness);
        movingPlatformPaths_.push_back({movingIndex, pointAX, movingPlatformY,
                                         pointBX, movingPlatformY, movingPlatformSpeed, true});
    }

    // TODO: replace generated staircase with hand-designed rooms later.
    for (int room = 1; room < roomCount; ++room) {
        const float roomTop = worldHeight_ - viewHeight_ * static_cast<float>(room + 1);
        const bool startLeft = room % 2 == 0;

        const float leftX = 120.0F;
        const float rightX = worldWidth_ - 120.0F - 220.0F;

        platforms_.emplace_back(startLeft ? leftX : rightX, roomTop + 700.0F, 220.0F, platformThickness);
        platforms_.emplace_back(startLeft ? rightX : leftX, roomTop + 460.0F, 220.0F, platformThickness);
        platforms_.emplace_back(worldWidth_ * 0.5F - 110.0F, roomTop + 220.0F, 220.0F, platformThickness);
    }
}

void ApexAscent::handleInput(Engine& engine)
{
    // Aim/charge only while grounded — no air control once jumping.
    float aim = 0.0F;
    if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)){
        aim -= 1.0F;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)){
        aim += 1.0F;
    }

    if (isOnGround_ && Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
        isCharging_ = true;
        aimDirection_ = aim;
    } else if (isCharging_ == false && aim != 0.0F){
        player_.setVelocityX(aim * walkSpeed);
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
    currentScaleMode_ = engine.getScaleMode();

    if (isCharging_) {
        chargePower_ += chargeSpeed * deltaTime;
        if (chargePower_ > maxChargePower) {
            chargePower_ = maxChargePower;
        }
    }

    // Kill leftover horizontal slide after landing.
    if (isOnGround_) {
        player_.setVelocityX(player_.getVelocityX() * std::pow(groundFriction, deltaTime));
    }

    updateMovingPlatforms(deltaTime);

    // Capture before this frame's movement for the landing check.
    previousPlayerBottom_ = player_.getY() + playerHeight;

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

bool ApexAscent::isMovingPlatform(const Entity& platform) const
{
    for (const MovingPlatformPath& path : movingPlatformPaths_) {
        if (&platform == &platforms_[path.platformIndex]) {
            return true;
        }
    }
    return false;
}

void ApexAscent::handleCollisions()
{
    isOnGround_ = false;
    groundedMovingPlatform_ = nullptr;

    for (Entity& platform : platforms_) {
        const Rect playerBounds = player_.getBounds();
        const Rect platformBounds = platform.getBounds();

        // Land when the foot crosses the top (avoids wrong-axis MTV near edges).
        const bool overlapsHorizontally =
            playerBounds.x < platformBounds.x + platformBounds.width &&
            playerBounds.x + playerBounds.width > platformBounds.x;

        if (player_.getVelocityY() > 0.0F && overlapsHorizontally &&
            previousPlayerBottom_ <= platformBounds.y + landingTolerance &&
            playerBounds.y + playerBounds.height >= platformBounds.y) {
            player_.setPosition(player_.getX(), platformBounds.y - playerHeight);
            player_.setVelocityY(0.0F);
            isOnGround_ = true;
            if (isMovingPlatform(platform)) {
                groundedMovingPlatform_ = &platform;
            }
            continue;
        }

        float pushX = 0.0F;
        float pushY = 0.0F;

        if (!Collision::getSeparation(player_, platform, pushX, pushY)) {
            continue;
        }

        if (pushY < 0.0F) {
            // Landed on top.
            isOnGround_ = true;
            if (isMovingPlatform(platform)) {
                groundedMovingPlatform_ = &platform;
            }
        }

        Collision::resolve(player_, platform);
    }
}

// Smooth vertical follow, clamped to the world.
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

void ApexAscent::updateMovingPlatforms(float deltaTime)
{
    for (MovingPlatformPath& path : movingPlatformPaths_) {
        Entity& platform = platforms_[path.platformIndex];
        const float prevX = platform.getX();
        const float prevY = platform.getY();

        const float targetX = path.movingToB ? path.pointBX : path.pointAX;
        const float targetY = path.movingToB ? path.pointBY : path.pointAY;
        const float dx = targetX - prevX;
        const float dy = targetY - prevY;
        const float distance = std::sqrt(dx * dx + dy * dy);
        const float step = path.speed * deltaTime;

        if (distance <= step || distance <= 0.0001F) {
            platform.setPosition(targetX, targetY);
            path.movingToB = !path.movingToB;
        } else {
            platform.setPosition(prevX + dx / distance * step, prevY + dy / distance * step);
        }

        // Carry the player with the platform they were standing on.
        if (&platform == groundedMovingPlatform_) {
            player_.setPosition(player_.getX() + (platform.getX() - prevX),
                                 player_.getY() + (platform.getY() - prevY));
        }
    }
}

void ApexAscent::render(SDL_Renderer* renderer) const
{
    // World draw shifted by camera.
    const auto drawRect = [this, renderer](const Rect& bounds, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        const SDL_FRect rect{bounds.x, bounds.y - camera_, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    };

    for (const Entity& platform : platforms_) {
        // Amber tint so moving platforms read as intentional.
        if (isMovingPlatform(platform)) {
            drawRect(platform.getBounds(), 200, 150, 60);
        } else {
            drawRect(platform.getBounds(), 90, 100, 130);
        }
    }

    drawRect(player_.getBounds(), 240, 240, 255);

    // Charge meter in screen space (not world space).
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

    const char* modeLabel =
        currentScaleMode_ == Engine::ScaleMode::Constant ? "Constant" : "Proportional";
    char scaleHud[64];
    std::snprintf(scaleHud, sizeof(scaleHud), "Scale: %s (F1 to toggle)", modeLabel);
    SDL_RenderDebugText(renderer, 20.0F, 40.0F, scaleHud);
}

} // namespace

int main()
{
    try {
        // 900x900 design resolution (fits ordinary screens in Constant mode).
        Engine engine("Apex Ascent", 900, 900);
        engine.setClearColor(18, 20, 32);

        ApexAscent game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
