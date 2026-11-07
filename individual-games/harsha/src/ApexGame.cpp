#include "ApexGame.hpp"

#include "ApexLevel.hpp"
#include "ApexNetworkConfig.hpp"
#include "ApexPresence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

namespace {
constexpr const char* LedgeCrumbleEvent = "LedgeCrumbleEvent";
constexpr const char* SummitEvent = "SummitEvent";
constexpr float noticeSeconds = 4.0F;
constexpr float ghostFlashSeconds = 0.6F;
constexpr std::int64_t replayFlashRealTics = 300'000'000;
} // namespace

ApexGame::ApexGame(Engine& engine, std::unique_ptr<Multiplayer::Session> session)
    : engine_(engine),
      replay_(engine.events(), engine.gameTime(), engine.realTime()),
      session_(std::move(session)),
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

    world_.setEventManager(&engine_.events());
    // F1 arrives as an InputEvent in onInput(), not through the engine's polling.
    engine_.setScaleToggleKey(SDL_SCANCODE_UNKNOWN);
    engine_.registerChord("dash-left", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_A});
    engine_.registerChord("dash-left", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LEFT});
    engine_.registerChord("dash-right", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_D});
    engine_.registerChord("dash-right", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RIGHT});
    subscriptions_.push_back(engine_.events().subscribe(Events::Input, [this](const Event& event) { onInput(event); }));
    subscriptions_.push_back(engine_.events().subscribe(Events::Collision, [this](const Event& event) { onCollision(event); }));
    subscriptions_.push_back(engine_.events().subscribe(Events::Death, [this](const Event& event) { onDeath(event); }));
    subscriptions_.push_back(engine_.events().subscribe(Events::Spawn, [this](const Event& event) { onSpawn(event); }));
    subscriptions_.push_back(engine_.events().subscribe(SummitEvent, [this](const Event& event) { onSummit(event); }));
    // After replay_'s own subscription, so it has stopped by the time this runs.
    // A stale stop can arrive for a playback that restarted, hence the check.
    subscriptions_.push_back(engine_.events().subscribe(Events::ReplayPlayStop, [this](const Event&) {
        if (!replay_.isPlaying()) {
            killCamPlaying_ = false;
            engine_.events().raise(Event{Events::ReplayRecordStart});
        }
    }));
    replay_.setRollingWindow(killCamTics);
    replay_.setFrameStateHook([this] { return recordClimbers(); });
    replay_.events().subscribe(Events::Death, [this](const Event& event) {
        if (event.origin == 0) {
            replayFlashUntil_ = engine_.realTime().now() + replayFlashRealTics;
        }
    });
    engine_.events().raise(Event{Events::ReplayRecordStart});

    if (session_ && session_->supportsEvents()) {
        bridge_ = std::make_unique<NetworkEventBridge>(
            engine_.events(), *session_,
            std::vector<std::string>{LedgeCrumbleEvent, Events::Death, SummitEvent});
    }

    player_ = &ApexLevel::makePlayer(world_, 0.0F, 0.0F, playerWidth, playerHeight, gravity);
    playerTransform_ = player_->get<Transform>();
    playerMotion_ = player_->get<Motion>();
    playerGravity_ = player_->get<Gravity>();
    const std::vector<GameObject*> windZones = world_.findAllWith<WindZone>();
    windZoneObject_ = windZones.empty() ? nullptr : windZones.front();
    crumbles_ = world_.findAllWith<Crumble>();
    // A ledge's number is its position in crumbles_: creation order, the same on
    // every client. Another player chooses the number, and a bad one matches no
    // ledge. Not guarded during a replay: this is live state, and the timer waits
    // for the replay's end. A broken ledge is inactive and already ignored this.
    for (std::size_t index = 0; index < crumbles_.size(); ++index) {
        Crumble* crumble = crumbles_[index]->get<Crumble>();
        crumbles_[index]->add<EventListener>(engine_.events()).listen(LedgeCrumbleEvent, [crumble, index](const Event& event) {
            if (event.get<std::int64_t>("ledge") == static_cast<std::int64_t>(index) &&
                crumble->state == Crumble::State::Intact) {
                crumble->state = Crumble::State::Shaking;
                crumble->timer = 0.0F;
            }
        });
    }

    // Start on room 0's first slot (online, the real slot follows once the
    // server assigns an id).
    checkpoint_ = findSpawnMarker(0, 0);
    raiseSpawnAt(*checkpoint_);

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
                     "A/D aim, Space charge, Left Shift + direction dashes in the air. "
                     "P pause, 1/2/3 = 0.5x/1x/2x game time. Esc quit.\n";
    } else {
        std::cout << "Apex Ascent: A/D to aim, hold Space to charge a jump, "
                     "release to leap, Left Shift + direction to dash in the air. "
                     "P pause, 1/2/3 = 0.5x/1x/2x game time. "
                     "F1 to toggle scaling, Esc to quit.\n"
                     "Optional: ./apex-ascent --join [tcp://host:port]\n";
    }
}

ApexGame::~ApexGame()
{
    for (const EventManager::SubscriptionId id : subscriptions_) {
        engine_.events().unsubscribe(id);
    }
}

// Key changes and the one-shot keys. Runs at the engine's first dispatch, also
// while paused, so P can unpause and a release during a pause is not missed.
void ApexGame::onInput(const Event& event)
{
    if (event.get<std::string>("kind") == "chord") {
        if (event.get<std::string>("state") == "pressed") {
            tryDash(event.get<std::string>("name") == "dash-left" ? -1.0F : 1.0F);
        }
        return;
    }
    const auto key = static_cast<SDL_Scancode>(event.get<std::int64_t>("key"));
    const bool pressed = event.get<std::string>("state") == "pressed";
    keysHeld_[key] = pressed;
    if (!pressed) {
        return;
    }

    if (key == SDL_SCANCODE_ESCAPE) {
        engine_.quit();
        return;
    }
    if (key == SDL_SCANCODE_F1) {
        engine_.toggleScaleMode();
        return;
    }
    if (key == SDL_SCANCODE_F5) {
        engine_.events().raise(Event{Events::ReplayPlayStart});
        return;
    }
    if (key == SDL_SCANCODE_F8) {
        engine_.events().raise(Event{Events::ReplaySpeed}.set(
            "speed", replay_.speed() < 1.0 ? 1.0 : replay_.speed() < 2.0 ? 2.0 : 0.5));
        return;
    }
    if (replay_.isPlaying()) {
        if (key == SDL_SCANCODE_F6) {
            engine_.events().raise(Event{Events::ReplayPause});
        } else if (key == SDL_SCANCODE_F7) {
            engine_.events().raise(Event{Events::ReplayPlayStop});
        }
        // The replay owns the live timeline's pause while it plays.
        return;
    }

    // Local game timeline, offline and online (online it does not affect peers or server).
    Timeline& gameTime = engine_.gameTime();
    switch (key) {
    case SDL_SCANCODE_P:
        gameTime.togglePause();
        break;
    case SDL_SCANCODE_1:
        gameTime.setScale(0.5);
        break;
    case SDL_SCANCODE_2:
        gameTime.setScale(1.0);
        break;
    case SDL_SCANCODE_3:
        gameTime.setScale(2.0);
        break;
    default:
        break;
    }
}

// What the held keys mean this frame. At the start of update(), not in
// handleInput(), which runs before the dispatch that delivers this frame's keys.
void ApexGame::applyHeldKeys(float deltaTime)
{
    // Walking is recomputed from the keys every frame; nothing held, no walk.
    walkIntent_ = 0.0F;

    // Paused or dead: freeze the climber.
    if (gameTimePaused_ || isDead_) {
        return;
    }

    updateDash(deltaTime);

    float aim = 0.0F;
    if (isHeld(SDL_SCANCODE_A) || isHeld(SDL_SCANCODE_LEFT)) {
        aim -= 1.0F;
    }
    if (isHeld(SDL_SCANCODE_D) || isHeld(SDL_SCANCODE_RIGHT)) {
        aim += 1.0F;
    }

    if (isOnGround_ && isHeld(SDL_SCANCODE_SPACE)) {
        isCharging_ = true;
        aimDirection_ = aim;
    } else if (!isCharging_ && isOnGround_) {
        // On the ground the player worker turns this into speed, because how
        // fast you get going depends on the surface (ice).
        walkIntent_ = aim;
    } else if (!isCharging_ && aim != 0.0F && dashTimeLeft_ <= 0.0F) {
        // Air control (the user's choice over design §3's "none"): holding a
        // direction mid-air sets sideways speed to walk speed. It cannot add
        // height, which is what room 3's vertical slingshot relies on.
        playerMotion_->velocityX = aim * walkSpeed;
    }

    // "Not held" rather than "just released": a release during a pause is
    // missed, so the stored charge fires on the first unpaused frame.
    if (isCharging_ && !isHeld(SDL_SCANCODE_SPACE)) {
        isCharging_ = false;

        const float power = baseJumpSpeed + chargePower_;
        playerMotion_->velocityX = aimDirection_ * power * horizontalJumpRatio;
        playerMotion_->velocityY = -power;
        applySlingshotFling();

        chargePower_ = 0.0F;
        isOnGround_ = false;
    }
}

void ApexGame::tryDash(float direction)
{
    if (engine_.gameTime().isPaused() || isDead_ || isOnGround_ || isCharging_ || dashUsed_) {
        return;
    }
    dashUsed_ = true;
    dashDirection_ = direction;
    dashTimeLeft_ = dashSeconds;
    playerMotion_->velocityX = direction * dashSpeed;
}

void ApexGame::updateDash(float deltaTime)
{
    if (dashTimeLeft_ > 0.0F) {
        dashTimeLeft_ = isOnGround_ ? 0.0F : dashTimeLeft_ - deltaTime;
        // There is no air drag, so without this it would keep flying at dash
        // speed, or slide off an ice ledge after landing mid-dash.
        if (dashTimeLeft_ <= 0.0F) {
            playerMotion_->velocityX = dashDirection_ * walkSpeed;
        }
    }
    if (isOnGround_) {
        dashUsed_ = false;
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

    replay_.update();

    // A replay pauses the live timeline, so the rest of this function treats it
    // as a pause: the session, bridge and pose keep going, and Carrier keeps
    // measuring, so a rider is not thrown when the replay ends.
    const Timeline& gameTime = engine.gameTime();
    gameTimePaused_ = gameTime.isPaused();
    gameTimeScale_ = gameTime.scale();

    applyHeldKeys(deltaTime);

    // Refresh snapshots, then mirror them into the World (server platform
    // poses, remote players) before anything reads positions this frame.
    // NetworkClient uses realTime, so pause/scale never freezes the link.
    if (session_) {
        session_->update();
        if (bridge_) {
            bridge_->apply();
        }
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
                    raiseSpawnAt(*slot);
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
        updateCrumbles(deltaTime);
        if (!summitAnnounced_ && isOnGround_) {
            const GameObject* grounded = world_.find(groundedPlatformId_);
            const ApexPlatform* kind = grounded != nullptr ? grounded->get<ApexPlatform>() : nullptr;
            if (kind != nullptr && kind->kind == ApexPlatform::Kind::Summit) {
                summitAnnounced_ = true;
                engine_.events().raise(Event{SummitEvent});
            }
        }
    }
    updateOnlineCues(deltaTime);
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
    replay_.capture(world_, 0.0F, camera_);

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

    // Still run the engine's pass, so switching the climber off reports its
    // exits (the wind zone). There is no one to land.
    if (isDead_) {
        world_.resolveCollisions();
        return;
    }

    const auto overlapsHorizontally = [](const Rect& a, const Rect& b) {
        return a.x < b.x + b.width && a.x + a.width > b.x;
    };

    // Landing pass first, over every platform: snap onto any top the feet
    // crossed this frame.
    const std::vector<GameObject*> platforms = world_.findAllByTag("platform");
    for (GameObject* platform : platforms) {
        // findAllByTag also returns switched-off objects: a crumbled ledge.
        if (!platform->isActive()) {
            continue;
        }
        const Rect playerBounds = playerTransform_->bounds();
        const Rect platformBounds = platform->get<Transform>()->bounds();

        // vy >= 0 (not only > 0): resting contact has vy == 0 and feet on the
        // surface with no AABB overlap, so MTV never fires; requiring a fall
        // made isOnGround_ flicker every other frame (anim Land/air twitch).
        // Land when the foot crosses the top (avoids wrong-axis MTV near edges).
        if (playerMotion_->velocityY >= 0.0F && overlapsHorizontally(playerBounds, platformBounds) &&
            previousPlayerBottom_ <= platformBounds.y + landingTolerance &&
            playerBounds.y + playerBounds.height >= platformBounds.y) {
            playerTransform_->y = platformBounds.y - playerHeight;
            playerMotion_->velocityY = 0.0F;
            isOnGround_ = true;
            groundedPlatformId_ = platform->id();
        }
    }

    // Then the engine's pass: pushes the player out of walls and ceilings
    // (MTV), and queues the collision events onCollision() handles later.
    world_.resolveCollisions();

    // A push that left the feet on a top counts as a landing. Checked here, not
    // from an event, because crumbles and animation read it this frame.
    const Rect playerBounds = playerTransform_->bounds();
    for (GameObject* platform : platforms) {
        if (!platform->isActive()) {
            continue;
        }
        const Rect platformBounds = platform->get<Transform>()->bounds();
        if (playerMotion_->velocityY >= 0.0F && overlapsHorizontally(playerBounds, platformBounds) &&
            std::fabs(playerBounds.y + playerBounds.height - platformBounds.y) <= pushLandingTolerance) {
            isOnGround_ = true;
            groundedPlatformId_ = platform->id();
        }
    }
}

// The climber's contacts, at the engine's dispatch after update(). Movers
// overlapping level geometry raise events too, so anything else is ignored.
void ApexGame::onCollision(const Event& event)
{
    const std::int64_t a = event.get<std::int64_t>("a");
    const std::int64_t b = event.get<std::int64_t>("b");
    const auto playerId = static_cast<std::int64_t>(player_->id());
    if (a != playerId && b != playerId) {
        return;
    }
    GameObject* other = world_.find(static_cast<ObjectId>(a == playerId ? b : a));
    if (other == nullptr) {
        return;
    }
    const std::string phase = event.get<std::string>("phase");

    if (const WindZone* zone = other->get<WindZone>()) {
        if (phase == "enter") {
            wind_ = zone;
        } else if (phase == "exit" && wind_ == zone) {
            wind_ = nullptr;
        }
        return;
    }
    if (phase == "exit") {
        return;
    }
    if (const DeathZone* death = other->get<DeathZone>()) {
        if (phase == "enter") {
            engine_.events().raise(Event{Events::Death}.set("object", player_->id())
                                                       .set("cause", death->spikes ? "spikes" : "fall"));
        }
        return;
    }
    // Standing in a higher room's checkpoint marker makes it the respawn
    // point. Grounded only: jumping through one on the way past does not count.
    if (const SpawnMarker* marker = other->get<SpawnMarker>()) {
        if (isOnGround_ && marker->room > checkpointRoom_) {
            checkpointRoom_ = marker->room;
            checkpoint_ = other->get<Transform>();
        }
        return;
    }
    // Room-snap camera: show the room whose zone holds the climber's centre.
    // A climber straddling two rooms overlaps both zones; the centre decides.
    if (const RoomZone* room = other->get<RoomZone>()) {
        const Transform& zone = *other->get<Transform>();
        const float centreY = playerTransform_->y + playerHeight * 0.5F;
        if (centreY >= zone.y && centreY < zone.y + zone.height) {
            cameraRoom_ = room->room;
        }
    }
}

void ApexGame::onDeath(const Event& event)
{
    // Another climber's death is only news: it never touches this climber. The
    // cause is compared, not shown, because another player chooses the text.
    if (event.origin != 0) {
        const char* where = event.getOr<std::string>("cause", "") == "spikes" ? " on the spikes" : "";
        showNotice("Climber #" + std::to_string(event.origin) + " fell" + where);
        if (sync_ && sync_->remotePlayer(event.origin) != nullptr) {
            ghostFlashLeft_[event.origin] = ghostFlashSeconds;
        }
        return;
    }
    // Touching two death zones in one frame is still one death.
    if (isDead_) {
        return;
    }
    isDead_ = true;
    ++deaths_;
    isCharging_ = false;
    chargePower_ = 0.0F;
    isOnGround_ = false;
    groundedPlatformId_ = 0;
    player_->setActive(false);
    // The spawn is on game time, which the replay pauses, so the climber comes
    // back after the kill cam, not under it.
    raiseSpawnAt(*checkpoint_, respawnDelayTics);
    killCamPlaying_ = true;
    engine_.events().raise(Event{Events::ReplayPlayStart});
}

void ApexGame::onSpawn(const Event& event)
{
    if (event.origin != 0) {
        return;
    }
    const auto x = static_cast<float>(event.get<double>("x"));
    const auto y = static_cast<float>(event.get<double>("y"));
    if (event.getOr<std::string>("kind", "") == "debris") {
        spawnDebris(Rect{x, y, static_cast<float>(event.get<double>("width")), 0.0F});
        return;
    }
    placeAt(x, y);
    isDead_ = false;
    player_->setActive(true);
}

void ApexGame::raiseSpawnAt(const Transform& marker, std::int64_t delayTics)
{
    Event spawn{Events::Spawn};
    spawn.set("object", player_->id())
        .set("x", static_cast<double>(marker.x + (marker.width - playerWidth) * 0.5F))
        .set("y", static_cast<double>(marker.y));
    // A full queue dropping this would leave the climber dead for good.
    spawn.priority = EventPriority::High;
    engine_.events().raise(std::move(spawn), delayTics);
}

void ApexGame::placeAt(float x, float y)
{
    playerTransform_->x = x;
    playerTransform_->y = y;
    playerMotion_->velocityX = 0.0F;
    playerMotion_->velocityY = 0.0F;
    previousPlayerBottom_ = y + playerHeight;
    dashUsed_ = false;
    dashTimeLeft_ = 0.0F;
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

// Crumbling ledges, on the main thread after the collision pass (so this
// frame's grounded platform is known). Standing on one starts a shake; when
// it ends the ledge breaks into debris and is switched off, then grows back.
void ApexGame::updateCrumbles(float deltaTime)
{
    const Color moss = ApexLevel::colorFor(ApexPlatform::Kind::Moss);
    const Color rotten = crumbledColor;

    for (std::size_t index = 0; index < crumbles_.size(); ++index) {
        GameObject* ledge = crumbles_[index];
        Crumble& crumble = *ledge->get<Crumble>();
        Transform& transform = *ledge->get<Transform>();
        Renderable& renderable = *ledge->get<Renderable>();

        switch (crumble.state) {
        case Crumble::State::Intact:
            // Only the ledge's listener starts the shake, so a landing here and one
            // on another client's screen take the same path.
            if (isOnGround_ && groundedPlatformId_ == ledge->id()) {
                engine_.events().raise(Event{LedgeCrumbleEvent}.set("ledge", static_cast<std::int64_t>(index)));
            }
            break;

        case Crumble::State::Shaking: {
            crumble.timer += deltaTime;
            const float progress = std::min(crumble.timer / Crumble::breakAfterSeconds, 1.0F);
            // A tremor that grows as the ledge weakens.
            transform.x = crumble.homeX + std::sin(crumble.timer * 70.0F) * (0.5F + 1.5F * progress);
            const auto mix = [progress](std::uint8_t from, std::uint8_t to) {
                return static_cast<std::uint8_t>(static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * progress);
            };
            renderable.color = {mix(moss.red, rotten.red), mix(moss.green, rotten.green), mix(moss.blue, rotten.blue)};

            if (crumble.timer >= Crumble::breakAfterSeconds) {
                transform.x = crumble.homeX;
                renderable.color = moss;
                Event debris{Events::Spawn};
                debris.set("kind", "debris")
                    .set("x", static_cast<double>(transform.x))
                    .set("y", static_cast<double>(transform.y))
                    .set("width", static_cast<double>(transform.width));
                // Only looks: anything else due in the same round goes first.
                debris.priority = EventPriority::Low;
                engine_.events().raise(std::move(debris));
                ledge->setActive(false);
                crumble.state = Crumble::State::Broken;
                crumble.timer = 0.0F;
                // The climber drops with it. A charge in progress is lost, or
                // releasing Space would leap from mid-air.
                if (groundedPlatformId_ == ledge->id()) {
                    isOnGround_ = false;
                    groundedPlatformId_ = 0;
                    isCharging_ = false;
                    chargePower_ = 0.0F;
                }
            }
            break;
        }

        case Crumble::State::Broken: {
            crumble.timer += deltaTime;
            // Grow back only once the space is clear, so it never closes
            // around a climber passing through.
            const Rect playerBounds = playerTransform_->bounds();
            const Rect ledgeBounds = transform.bounds();
            const bool blocked = playerBounds.x < ledgeBounds.x + ledgeBounds.width &&
                                 playerBounds.x + playerBounds.width > ledgeBounds.x &&
                                 playerBounds.y < ledgeBounds.y + ledgeBounds.height &&
                                 playerBounds.y + playerBounds.height > ledgeBounds.y;
            if (crumble.timer >= Crumble::returnAfterSeconds && !blocked) {
                ledge->setActive(true);
                crumble.state = Crumble::State::Intact;
                crumble.timer = 0.0F;
            }
            break;
        }
        }
    }

    // Debris falls under the climber's gravity and fades out.
    for (Debris& bit : debris_) {
        bit.velocityY += gravity * deltaTime;
        bit.x += bit.velocityX * deltaTime;
        bit.y += bit.velocityY * deltaTime;
        bit.life -= deltaTime;
    }
    debris_.erase(std::remove_if(debris_.begin(), debris_.end(), [](const Debris& bit) { return bit.life <= 0.0F; }),
                  debris_.end());
}

// This climber's own summit already shows the banner.
void ApexGame::onSummit(const Event& event)
{
    if (event.origin != 0) {
        showNotice("Climber #" + std::to_string(event.origin) + " reached the Apex!");
    }
}

// One line per climber: id (0 = this one), x, y and animation. A dead climber
// is hidden, so it is left out. Ghost animations were checked by decodeAnimation
// and are written back from the result, because another player chose the text.
std::string ApexGame::recordClimbers() const
{
    std::string text;
    if (!isDead_) {
        text += "0 " + std::to_string(playerTransform_->x) + ' ' + std::to_string(playerTransform_->y) + ' ' +
                ApexPresence::encodeAnimation(playerAnim_) + '\n';
    }
    if (session_) {
        for (const Multiplayer::Player& remote : session_->remotePlayers()) {
            ApexPresence::RemoteAnimation animation;
            ApexPresence::decodeAnimation(remote.data, animation);
            text += std::to_string(remote.id) + ' ' + std::to_string(remote.x) + ' ' + std::to_string(remote.y) + ' ' +
                    std::to_string(static_cast<int>(animation.clip)) + ' ' + std::to_string(animation.frame) + ' ' +
                    (animation.facingRight ? "1" : "0") + '\n';
        }
    }
    return text;
}

void ApexGame::showNotice(std::string text)
{
    notice_ = std::move(text);
    noticeSecondsLeft_ = noticeSeconds;
}

void ApexGame::updateOnlineCues(float deltaTime)
{
    noticeSecondsLeft_ = std::max(0.0F, noticeSecondsLeft_ - deltaTime);
    for (auto it = ghostFlashLeft_.begin(); it != ghostFlashLeft_.end();) {
        it->second -= deltaTime;
        it = it->second <= 0.0F ? ghostFlashLeft_.erase(it) : std::next(it);
    }
}

// Eight chunks spread along the ledge, kicked up and outward from its middle.
// Fixed spread rather than random, so every break looks the same.
void ApexGame::spawnDebris(const Rect& ledge)
{
    constexpr int pieces = 8;
    for (int i = 0; i < pieces; ++i) {
        const float along = (static_cast<float>(i) + 0.5F) / pieces; // 0..1 across the ledge
        Debris bit{};
        bit.size = 5.0F + static_cast<float>((i * 3) % 5);
        bit.x = ledge.x + along * ledge.width - bit.size * 0.5F;
        bit.y = ledge.y;
        bit.velocityX = (along - 0.5F) * 260.0F;
        bit.velocityY = -140.0F - 50.0F * static_cast<float>(i % 3);
        bit.life = 0.9F + 0.1F * static_cast<float>(i % 3);
        debris_.push_back(bit);
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
