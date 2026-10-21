#include "ApexGame.hpp"

#include "ApexLevel.hpp"
#include "ApexNetworkConfig.hpp"
#include "ApexPresence.hpp"
#include "Input.hpp"

#include <algorithm>
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
      currentScaleMode_(engine.getScaleMode()),
      // Disjoint tags, so the two workers never touch the same object.
      workers_({
          [this](float) { world_.updateComponents(frameTime_, "platform"); },
          [this](float dt) { updatePlayerPhysics(dt); },
      })
{
    ApexLevel::buildLevel(world_, worldWidth_, worldHeight_, viewHeight_, session_ != nullptr);

    player_ = &ApexLevel::makePlayer(world_, 0.0F, 0.0F, playerWidth, playerHeight, gravity);
    playerTransform_ = player_->get<Transform>();
    playerMotion_ = player_->get<Motion>();
    playerGravity_ = player_->get<Gravity>();
    const std::vector<GameObject*> windZones = world_.findAllWith<WindZone>();
    windZoneObject_ = windZones.empty() ? nullptr : windZones.front();

    Collider& playerCollider = *player_->get<Collider>();
    playerCollider.onCollide = [this](GameObject&, GameObject& other) { onPlayerCollide(other); };
    playerCollider.onEnter = [this](GameObject&, GameObject& other) { onPlayerEnter(other); };
    playerCollider.onExit = [this](GameObject&, GameObject& other) { onPlayerExit(other); };

    // Start on room 0's first slot (online, the real slot follows once the
    // server assigns an id).
    checkpoint_ = findSpawnMarker(0, 0);
    placeAt(*checkpoint_);

    // Online: other climbers and server-owned platforms become objects in the
    // same World, created, moved and removed from session snapshots.
    if (session_) {
        sync_ = std::make_unique<NetworkWorldSync>(
            world_,
            [](World& world, const Multiplayer::Player&) -> GameObject& {
                return ApexLevel::makeRemotePlayer(world, playerWidth, playerHeight);
            },
            [](World& world, const Multiplayer::Platform& platform) -> GameObject& {
                return ApexLevel::makeServerPlatform(world, platform.id);
            });
    }

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

    // Walking is recomputed from the keys every frame; nothing held, no walk.
    walkIntent_ = 0.0F;

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
    } else if (!isCharging_ && isOnGround_) {
        // On the ground the player worker turns this into speed, because how
        // fast you get going depends on the surface (ice).
        walkIntent_ = aim;
    } else if (!isCharging_ && aim != 0.0F) {
        // Air control (the user's choice over design §3's "none"): holding a
        // direction mid-air sets sideways speed to walk speed. It cannot add
        // height, which is what room 3's vertical slingshot relies on.
        playerMotion_->velocityX = aim * walkSpeed;
    }

    // "Not held" rather than "just released": a release during a pause is
    // missed, so the stored charge fires on the first unpaused frame.
    if (isCharging_ && !Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
        isCharging_ = false;

        const float power = baseJumpSpeed + chargePower_;
        playerMotion_->velocityX = aimDirection_ * power * horizontalJumpRatio;
        playerMotion_->velocityY = -power;
        applySlingshotFling();

        chargePower_ = 0.0F;
        isOnGround_ = false;
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
    }
}

// Jumping off a slingshot rail throws the climber: the rail's velocity is
// added to the leap (all of it sideways, half of it vertically). Ordinary
// movers only carry; they never fling. Online, Carrier's velocity is measured
// across snapshots and held between them, so the fling works there too.
void ApexGame::applySlingshotFling()
{
    if (groundedPlatformId_ == 0) {
        return;
    }
    const GameObject* platform = world_.find(groundedPlatformId_);
    if (platform == nullptr) {
        return;
    }
    const ApexPlatform* kind = platform->get<ApexPlatform>();
    const Carrier* carrier = platform->get<Carrier>();
    if (kind == nullptr || carrier == nullptr || kind->kind != ApexPlatform::Kind::Slingshot) {
        return;
    }
    playerMotion_->velocityX += carrier->velocityX;
    playerMotion_->velocityY += carrier->velocityY * slingshotVerticalShare;
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

        // Take this climber's own starting slot once the server has numbered
        // it, unless it has already started climbing.
        const Multiplayer::PlayerId id = session_->localPlayerId();
        if (!onlineSlotPlaced_ && id != 0) {
            onlineSlotPlaced_ = true;
            if (checkpointRoom_ == 0 && highestRoomReached_ == 0 && isOnGround_ && !isCharging_) {
                if (const Transform* slot = findSpawnMarker(0, static_cast<int>((id - 1) % 4))) {
                    checkpoint_ = slot;
                    placeAt(*slot);
                }
            }
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

    // Friction of whatever the climber stands on, read here on the main
    // thread so the player worker never touches a platform object.
    groundFriction_ = Surface::ground;
    if (const GameObject* platform = groundedPlatformId_ != 0 ? world_.find(groundedPlatformId_) : nullptr) {
        if (const Surface* surface = platform->get<Surface>()) {
            groundFriction_ = surface->friction;
        }
    }

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

    // Wind cue state for render(): the gust right now, and streak drift that
    // follows it (and stops while paused, like the gust itself).
    if (windZoneObject_ != nullptr) {
        windNow_ = windZoneObject_->get<WindZone>()->accelerationAt(static_cast<double>(time.gameTimeUs) / 1e6);
        windStreakOffset_ += windNow_ * 0.6F * deltaTime;
    }

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

    // roomAt clamps, so a leap above the tower's top can't count as room 7.
    const int room = roomAt(playerTransform_->y);
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

    // A death zone was touched: back to the checkpoint. Done here, after the
    // pass, rather than inside the callback, so every other callback this
    // frame saw the same climber position.
    if (pendingRespawn_) {
        pendingRespawn_ = false;
        ++deaths_;
        placeAt(*checkpoint_);
    }
}

// Player's onCollide. World calls it after all pushes, so the player's
// position is settled: if its feet now rest on this platform's top, the MTV
// pushed it up onto it (the old "pushY < 0" landing case).
void ApexGame::onPlayerCollide(GameObject& other)
{
    // Standing in a higher room's checkpoint marker makes it the respawn
    // point. Grounded only: jumping through one on the way past does not count.
    if (const SpawnMarker* marker = other.get<SpawnMarker>()) {
        if (isOnGround_ && marker->room > checkpointRoom_) {
            checkpointRoom_ = marker->room;
            checkpoint_ = other.get<Transform>();
        }
        return;
    }
    // Room-snap camera: show the room whose zone holds the climber's centre.
    // A climber straddling two rooms overlaps both zones; the centre decides.
    if (const RoomZone* room = other.get<RoomZone>()) {
        const Transform& zone = *other.get<Transform>();
        const float centreY = playerTransform_->y + playerHeight * 0.5F;
        if (centreY >= zone.y && centreY < zone.y + zone.height) {
            cameraRoom_ = room->room;
        }
        return;
    }
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

void ApexGame::placeAt(const Transform& marker)
{
    playerTransform_->x = marker.x + (marker.width - playerWidth) * 0.5F;
    playerTransform_->y = marker.y;
    playerMotion_->velocityX = 0.0F;
    playerMotion_->velocityY = 0.0F;
    previousPlayerBottom_ = marker.y + playerHeight;
    isCharging_ = false;
    chargePower_ = 0.0F;
    // The landing pass grounds the climber on the next frame: its feet are on
    // the ledge top and previousPlayerBottom_ says it came from there.
    isOnGround_ = false;
    groundedPlatformId_ = 0;
    snapCamera();
}

const Transform* ApexGame::findSpawnMarker(int room, int slot)
{
    for (GameObject* object : world_.findAllWith<SpawnMarker>()) {
        const SpawnMarker& marker = *object->get<SpawnMarker>();
        if (marker.room == room && marker.slot == slot) {
            return object->get<Transform>();
        }
    }
    return nullptr;
}

// Player's onEnter / onExit: hidden trigger zones the climber moves through.
void ApexGame::onPlayerEnter(GameObject& other)
{
    if (other.has<DeathZone>()) {
        pendingRespawn_ = true;
    }
    if (const WindZone* zone = other.get<WindZone>()) {
        wind_ = zone;
    }
}

void ApexGame::onPlayerExit(GameObject& other)
{
    if (other.get<WindZone>() == wind_) {
        wind_ = nullptr;
    }
}

// Which room a world y is in (0 = bottom), clamped to the tower.
int ApexGame::roomAt(float worldY) const
{
    const int room = static_cast<int>((worldHeight_ - worldY) / viewHeight_);
    return std::max(0, std::min(room, roomCount - 1));
}

// The top of the room on screen: each room is exactly one screen tall.
float ApexGame::cameraTarget() const
{
    return worldHeight_ - viewHeight_ * static_cast<float>(cameraRoom_ + 1);
}

// Straight to the climber's room, for spawns and respawns: the zones only
// report on the next collision pass, and a teleport should not pan.
void ApexGame::snapCamera()
{
    cameraRoom_ = roomAt(playerTransform_->y + playerHeight * 0.5F);
    camera_ = cameraTarget();
}

// Ease toward the current room's top: a quick slide (about a tenth of a
// second) instead of a hard cut, so the jump into a new room still reads.
void ApexGame::updateCamera(float deltaTime)
{
    camera_ += (cameraTarget() - camera_) * (1.0F - std::pow(0.0001F, deltaTime));
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

    // Ground speed: walking and sliding both depend on the surface.
    if (isOnGround_) {
        float& velocityX = playerMotion_->velocityX;
        const float keep = std::pow(groundFriction_, deltaTime);
        if (walkIntent_ != 0.0F && groundFriction_ > Surface::ground) {
            // Slippery: walking only pulls speed toward the walk speed, so
            // you take a moment to get going, and slide when you stop.
            const float target = walkIntent_ * walkSpeed;
            velocityX = target + (velocityX - target) * keep;
        } else {
            // Grippy ground (as before): walking sets the speed outright,
            // and friction kills any leftover slide almost at once.
            if (walkIntent_ != 0.0F) {
                velocityX = walkIntent_ * walkSpeed;
            }
            velocityX *= keep;
        }
    }

    // Wind pushes only while airborne: gusts throw a jump off line but never
    // shove a standing climber off a ledge.
    if (wind_ != nullptr && !isOnGround_) {
        const double seconds = static_cast<double>(frameTime_.gameTimeUs) / 1e6;
        playerMotion_->velocityX += wind_->accelerationAt(seconds) * deltaTime;
    }

    // Gravity, then Motion (their priority order) move the player.
    world_.updateComponents(frameTime_, "player");
}
