#include "ApexGame.hpp"

#include "ApexLevel.hpp"
#include "ApexNetworkConfig.hpp"
#include "ApexPresence.hpp"
#include "Input.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <utility>

ApexGame::ApexGame(const Engine& engine, std::unique_ptr<Multiplayer::Session> session)
    : session_(std::move(session)),
      viewWidth_(static_cast<float>(engine.getWidth())),
      viewHeight_(static_cast<float>(engine.getHeight())),
      worldWidth_(viewWidth_),
      worldHeight_(viewHeight_ * static_cast<float>(roomCount)),
      startX_(worldWidth_ * 0.5F - playerWidth * 0.5F),
      startY_(worldHeight_ - 140.0F - playerHeight),
      currentScaleMode_(engine.getScaleMode()),
      // Disjoint tags, so the two workers never touch the same object.
      workers_({
          [this](float) { world_.updateComponents(frameTime_, "platform"); },
          [this](float dt) { updatePlayerPhysics(dt); },
      })
{
    ApexLevel::buildLevel(world_, worldWidth_, worldHeight_, viewHeight_, session_ != nullptr);

    player_ = &ApexLevel::makePlayer(world_, startX_, startY_, playerWidth, playerHeight, gravity);
    playerTransform_ = player_->get<Transform>();
    playerMotion_ = player_->get<Motion>();
    playerGravity_ = player_->get<Gravity>();
    player_->get<Collider>()->onCollide = [this](GameObject&, GameObject& other) {
        onPlayerCollide(other);
    };
    previousPlayerBottom_ = startY_ + playerHeight;

    // Online: other climbers and server-owned platforms become objects in the
    // same World, created, moved and removed from session snapshots.
    if (session_) {
        sync_ = std::make_unique<NetworkWorldSync>(
            world_,
            [](World& world, const Multiplayer::Player&) -> GameObject& {
                return ApexLevel::makeRemotePlayer(world, playerWidth, playerHeight);
            },
            [](World& world, const Multiplayer::Platform&) -> GameObject& {
                return ApexLevel::makeServerPlatform(world);
            });
    }

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
                     "release to leap. P pause, 1/2/3 = 0.5x/1x/2x game time. "
                     "F1 to toggle scaling, Esc to quit.\n"
                     "Optional: ./apex-ascent --join [tcp://host:port]\n";
    }
}

void ApexGame::handleInput(Engine& engine)
{
    // Local game timeline, offline and online (online it does not affect peers or server).
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
        playerMotion_->velocityX = aim * walkSpeed;
    }

    // "Not held" rather than "just released": a release during a pause is
    // missed, so the stored charge fires on the first unpaused frame.
    if (isCharging_ && !Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
        isCharging_ = false;

        const float power = baseJumpSpeed + chargePower_;
        playerMotion_->velocityX = aimDirection_ * power * horizontalJumpRatio;
        playerMotion_->velocityY = -power;

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
        session_->publishLocalPlayer(playerTransform_->x, playerTransform_->y,
                                     ApexPresence::encodeAnimation(playerAnim_));
    }
}

// The engine only calls the FrameTime overload; this exists because Game
// declares it pure virtual.
void ApexGame::update(float deltaTime, Engine& engine)
{
    update(FrameTime{deltaTime, 0}, engine);
}

void ApexGame::update(const FrameTime& time, Engine& engine)
{
    const auto deltaTime = static_cast<float>(time.dtSeconds);
    currentScaleMode_ = engine.getScaleMode();

    const Timeline& gameTime = engine.gameTime();
    gameTimePaused_ = gameTime.isPaused();
    gameTimeScale_ = gameTime.scale();

    // Refresh snapshots, then mirror them into the World (server platform
    // poses, remote players) before anything reads positions this frame.
    // NetworkClient uses realTime, so pause/scale never freezes the link.
    if (session_) {
        session_->update();
        if (sync_) {
            sync_->apply(*session_);
        }

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

    // Capture before this frame's movement for the landing check.
    previousPlayerBottom_ = playerTransform_->y + playerHeight;

    // No gravity while standing: the landing pass holds the climber on the
    // surface. Set here, before the workers start, so no thread races on it.
    playerGravity_->acceleration = isOnGround_ ? 0.0F : gravity;

    // Platforms and player physics run in parallel, one worker each; this
    // returns once both are done, so everything below reads finished results.
    // The workers read frameTime_, which nothing writes until runFrame returns.
    frameTime_ = time;
    workers_.runFrame(deltaTime);

    // Ride the platform last, on this thread: the platform's Carrier only
    // measured how far it moved. Movement adds up, so applying it after the
    // player's own move lands in the same place as applying it before.
    // Not while paused: online, the server keeps moving the platform on real
    // time, but a paused climber must stay where it is.
    if (!gameTimePaused_ && groundedPlatformId_ != 0) {
        GameObject* platform = world_.find(groundedPlatformId_);
        const Carrier* carrier = platform != nullptr ? platform->get<Carrier>() : nullptr;
        if (carrier != nullptr) {
            playerTransform_->x += carrier->dx;
            playerTransform_->y += carrier->dy;
            previousPlayerBottom_ += carrier->dy;
        }
    }

    // Paused: the climber's grounded state and animation stay frozen too.
    // Otherwise a server platform sliding away (and back) while paused flips
    // the anim to Fall, then to a Land clip that cannot advance at dt 0.
    if (!gameTimePaused_) {
        handleCollisions();
    }
    // Online ghosts that disconnected were queued for removal by sync_.
    world_.flushDestroyed();

    updateCamera(deltaTime);

    if (!gameTimePaused_) {
        PlayerAnimation::AnimInput animInput;
        animInput.onGround = isOnGround_;
        animInput.charging = isCharging_;
        animInput.velocityX = playerMotion_->velocityX;
        animInput.velocityY = playerMotion_->velocityY;
        // Prefer aim while charging; otherwise use horizontal velocity.
        animInput.facingIntent = isCharging_ ? aimDirection_ : playerMotion_->velocityX;
        playerAnim_.update(deltaTime, animInput);
    }

    updateNetwork();

    const int room = static_cast<int>((worldHeight_ - playerTransform_->y) / viewHeight_);
    if (room > highestRoomReached_) {
        highestRoomReached_ = room;
    }
}

void ApexGame::handleCollisions()
{
    isOnGround_ = false;
    groundedPlatformId_ = 0;

    // Landing pass first, over every platform: snap onto any top the feet
    // crossed this frame.
    for (GameObject* platform : world_.findAllByTag("platform")) {
        const Rect playerBounds = playerTransform_->bounds();
        const Rect platformBounds = platform->get<Transform>()->bounds();

        // Land when the foot crosses the top (avoids wrong-axis MTV near edges).
        const bool overlapsHorizontally =
            playerBounds.x < platformBounds.x + platformBounds.width &&
            playerBounds.x + playerBounds.width > platformBounds.x;

        // vy >= 0 (not only > 0): resting contact has vy == 0 and feet on the
        // surface with no AABB overlap, so MTV never fires; requiring a fall
        // made isOnGround_ flicker every other frame (anim Land/air twitch).
        if (playerMotion_->velocityY >= 0.0F && overlapsHorizontally &&
            previousPlayerBottom_ <= platformBounds.y + landingTolerance &&
            playerBounds.y + playerBounds.height >= platformBounds.y) {
            playerTransform_->y = platformBounds.y - playerHeight;
            playerMotion_->velocityY = 0.0F;
            isOnGround_ = true;
            groundedPlatformId_ = platform->id();
        }
    }

    // Then the engine's pass: pushes the player out of walls and ceilings
    // (MTV), and calls collider callbacks. A push that leaves the player on
    // top of a platform counts as a landing; see onPlayerCollide.
    world_.resolveCollisions();
}

// Player's onCollide. World calls it after all pushes, so the player's
// position is settled: if its feet now rest on this platform's top, the MTV
// pushed it up onto it (the old "pushY < 0" landing case).
void ApexGame::onPlayerCollide(GameObject& other)
{
    if (other.tag() != "platform") {
        return;
    }
    const float playerBottom = playerTransform_->y + playerHeight;
    const float platformTop = other.get<Transform>()->y;
    if (playerMotion_->velocityY >= 0.0F && std::fabs(playerBottom - platformTop) <= landingTolerance) {
        isOnGround_ = true;
        groundedPlatformId_ = other.id();
    }
}

// Smooth vertical follow, clamped to the world.
void ApexGame::updateCamera(float deltaTime)
{
    const float target = playerTransform_->y + playerHeight * 0.5F - viewHeight_ * 0.5F;
    camera_ += (target - camera_) * (1.0F - std::pow(0.001F, deltaTime));

    if (camera_ < 0.0F) {
        camera_ = 0.0F;
    }
    if (camera_ > worldHeight_ - viewHeight_) {
        camera_ = worldHeight_ - viewHeight_;
    }
}

// Player worker thread: touches only the player and its charge, never a
// platform. The other worker is updating the "platform" objects meanwhile.
void ApexGame::updatePlayerPhysics(float deltaTime)
{
    if (isCharging_) {
        chargePower_ += chargeSpeed * deltaTime;
        if (chargePower_ > maxChargePower) {
            chargePower_ = maxChargePower;
        }
    }

    // Kill leftover horizontal slide after landing.
    if (isOnGround_) {
        playerMotion_->velocityX *= std::pow(Surface::ground, deltaTime);
    }

    // Gravity, then Motion (their priority order) move the player.
    world_.updateComponents(frameTime_, "player");
}
