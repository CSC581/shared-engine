#include "ApexGame.hpp"

#include "ApexNetworkConfig.hpp"
#include "ApexPresence.hpp"
#include "Collision.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <utility>

ApexGame::ApexGame(const Engine& engine, std::unique_ptr<Multiplayer::Session> session)
    : player_(0.0F, 0.0F, playerWidth, playerHeight),
      session_(std::move(session)),
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

    if (!playerAnim_.load(engine.getRenderer())) {
        std::cerr << "Apex Ascent: player sprite load failed; falling back to rect draw "
                     "(expected media/apex-ascent/Idle.png next to the binary).\n";
    }

    if (session_) {
        std::cout << "Apex Ascent (" << Multiplayer::modeName(session_->mode())
                  << "): peers are animated ghosts; moving platform is authority-owned.\n"
                     "A/D aim, Space charge. P pause, 1/2/3 = 0.5x/1x/2x game time. Esc quit.\n";
    } else {
        std::cout << "Apex Ascent: A/D to aim, hold Space to charge a jump, "
                     "release to leap. F1 to toggle scaling, Esc to quit.\n"
                     "Optional: ./apex-ascent --join [tcp://host:port]\n";
    }
}

void ApexGame::buildLevel()
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

    // Auto-moving platform (matches ApexNetwork::makeServerConfig for online).
    {
        const float movingPlatformY = ApexNetwork::movingPlatformY();
        const std::size_t movingIndex = platforms_.size();
        platforms_.emplace_back(ApexNetwork::movingPointAX, movingPlatformY,
                                ApexNetwork::movingPlatformWidth, platformThickness);
        movingPlatformPaths_.push_back({movingIndex, ApexNetwork::movingPointAX, movingPlatformY,
                                         ApexNetwork::movingPointBX, movingPlatformY,
                                         ApexNetwork::movingPlatformSpeed, true,
                                         ApexNetwork::movingPlatformId, false});
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

void ApexGame::handleInput(Engine& engine)
{
    // Online-only: local game timeline isolation (does not affect peers or server).
    if (session_) {
        Timeline& gameTime = engine.gameTime();
        if (Input::isKeyJustPressed(SDL_SCANCODE_P)) {
            gameTime.togglePause();
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_1)) {
            gameTime.setScale(0.5);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_2)) {
            gameTime.setScale(1.0);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_3)) {
            gameTime.setScale(2.0);
        }

        // Paused: freeze the climber. Only quit and the timeline keys above act.
        if (gameTime.isPaused()) {
            if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
                engine.quit();
            }
            return;
        }
    }

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

    // "Not held" rather than "just released": a release during a pause is
    // missed, so the stored charge fires on the first unpaused frame.
    if (isCharging_ && !Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
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

void ApexGame::updateNetwork()
{
    if (!session_) {
        return;
    }

    // update() is called at the start of the frame so platform poses are fresh.
    if (session_->state() == Multiplayer::State::Ready) {
        // Local climb is authoritative; never rewind from snapshot (server spawn
        // is only a join hint — we keep room-0 placement from the client).
        session_->publishLocalPlayer(player_.getX(), player_.getY(),
                                     ApexPresence::encodeAnimation(playerAnim_));
    }
}

void ApexGame::update(float deltaTime, Engine& engine)
{
    currentScaleMode_ = engine.getScaleMode();

    // Refresh snapshots before moving-platform carry (server poses).
    // NetworkClient uses realTime, so pause/scale never freezes the link.
    if (session_) {
        session_->update();

        const Timeline& gameTime = engine.gameTime();
        gameTimePaused_ = gameTime.isPaused();
        gameTimeScale_ = gameTime.scale();

        char title[128];
        if (gameTimePaused_) {
            std::snprintf(title, sizeof(title), "Apex Ascent (online) | PAUSED | #%u",
                          static_cast<unsigned>(session_->localPlayerId()));
        } else {
            std::snprintf(title, sizeof(title), "Apex Ascent (online) | %.1fx | #%u",
                          gameTimeScale_, static_cast<unsigned>(session_->localPlayerId()));
        }
        SDL_SetWindowTitle(engine.getWindow(), title);
    }

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

    PlayerAnimation::AnimInput animInput;
    animInput.onGround = isOnGround_;
    animInput.charging = isCharging_;
    animInput.velocityX = player_.getVelocityX();
    animInput.velocityY = player_.getVelocityY();
    // Prefer aim while charging; otherwise use horizontal velocity.
    animInput.facingIntent = isCharging_ ? aimDirection_ : player_.getVelocityX();
    playerAnim_.update(deltaTime, animInput);

    updateNetwork();

    const int room = static_cast<int>((worldHeight_ - player_.getY()) / viewHeight_);
    if (room > highestRoomReached_) {
        highestRoomReached_ = room;
    }
}

bool ApexGame::isMovingPlatform(const Entity& platform) const
{
    for (const MovingPlatformPath& path : movingPlatformPaths_) {
        if (&platform == &platforms_[path.platformIndex]) {
            return true;
        }
    }
    return false;
}

void ApexGame::handleCollisions()
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

        // vy >= 0 (not only > 0): resting contact has vy == 0 and feet on the
        // surface with no AABB overlap, so MTV never fires; requiring a fall
        // made isOnGround_ flicker every other frame (anim Land/air twitch).
        if (player_.getVelocityY() >= 0.0F && overlapsHorizontally &&
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
void ApexGame::updateCamera(float deltaTime)
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

void ApexGame::updateMovingPlatforms(float deltaTime)
{
    // Online + connected: server owns networked movers (no local path advance).
    if (session_ && session_->state() == Multiplayer::State::Ready) {
        applyServerMovingPlatforms();
        return;
    }

    for (MovingPlatformPath& path : movingPlatformPaths_) {
        path.serverPoseInitialized = false;

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

void ApexGame::applyServerMovingPlatforms()
{
    for (MovingPlatformPath& path : movingPlatformPaths_) {
        if (path.networkId == 0) {
            continue;
        }

        const Multiplayer::Platform* match = nullptr;
        for (const Multiplayer::Platform& platform : session_->platforms()) {
            if (platform.id == path.networkId) {
                match = &platform;
                break;
            }
        }
        if (match == nullptr) {
            continue;
        }

        Entity& platform = platforms_[path.platformIndex];
        const float prevX = platform.getX();
        const float prevY = platform.getY();
        platform.setPosition(match->x, match->y);
        platform.setSize(match->width, match->height);

        // First snapshot after connect only snaps pose; later frames carry by delta.
        if (path.serverPoseInitialized && &platform == groundedMovingPlatform_) {
            player_.setPosition(player_.getX() + (platform.getX() - prevX),
                                 player_.getY() + (platform.getY() - prevY));
        }
        path.serverPoseInitialized = true;
    }
}
