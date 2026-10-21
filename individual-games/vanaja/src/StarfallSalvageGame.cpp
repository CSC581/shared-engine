#include "StarfallSalvageGame.hpp"

#include "Collision.hpp"
#include "Components.hpp"
#include "Engine.hpp"
#include "Input.hpp"
#include "StarfallSalvageWorld.hpp"
#include "TimeUnits.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr std::array<Color, 8> suits{{
    {105, 225, 245}, {255, 185, 95}, {245, 115, 155}, {154, 224, 148},
    {180, 161, 255}, {245, 210, 110}, {105, 170, 255}, {235, 150, 210},
}};

void rect(SDL_Renderer* renderer, float x, float y, float w, float h, Color color)
{
    const SDL_FRect box{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
    SDL_RenderFillRect(renderer, &box);
}

void worldRect(SDL_Renderer* renderer, float cameraX, float x, float y, float w,
               float h, Color color)
{
    rect(renderer, x - cameraX, y, w, h, color);
}

void line(SDL_Renderer* renderer, float x1, float y1, float x2, float y2, Color color)
{
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
    SDL_RenderLine(renderer, x1, y1, x2, y2);
}

void worldLine(SDL_Renderer* renderer, float cameraX, float x1, float y1, float x2,
               float y2, Color color)
{
    line(renderer, x1 - cameraX, y1, x2 - cameraX, y2, color);
}

void worldRing(SDL_Renderer* renderer, float cameraX, float centerX, float centerY,
               float radius, Color color)
{
    constexpr int segments = 48;
    constexpr float step = 6.2831853F / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a = step * static_cast<float>(i);
        const float b = a + step;
        worldLine(renderer, cameraX, centerX + radius * std::cos(a), centerY + radius * std::sin(a),
                  centerX + radius * std::cos(b), centerY + radius * std::sin(b), color);
    }
}

Color suitFor(std::uint32_t id)
{
    return suits[(id ? id - 1 : 0) % suits.size()];
}

void astronaut(SDL_Renderer* renderer, Rect bounds, Color suit, const std::string& name,
               float cameraX, float stride = 0.0F, bool airborne = false,
               bool shielded = false)
{
    bounds.x -= cameraX;
    if (airborne) {
        rect(renderer, bounds.x + 8, bounds.y + 40, 5, 7, {98, 219, 246});
        rect(renderer, bounds.x + bounds.width - 13, bounds.y + 40, 5, 7,
             {98, 219, 246});
    }
    rect(renderer, bounds.x + 2, bounds.y + 13, bounds.width - 4, 24, {17, 41, 61});
    rect(renderer, bounds.x + 4, bounds.y + 15, bounds.width - 8, 20, suit);
    rect(renderer, bounds.x + 1, bounds.y + 3, bounds.width - 2, 19, {207, 238, 244});
    rect(renderer, bounds.x + 4, bounds.y + 6, bounds.width - 8, 11, {28, 82, 111});
    rect(renderer, bounds.x + 7, bounds.y + 8, 6, 2, {136, 233, 250});
    rect(renderer, bounds.x + 2, bounds.y + 35 + stride, 10, 5, {63, 109, 139});
    rect(renderer, bounds.x + bounds.width - 12, bounds.y + 35 - stride, 10, 5,
         {63, 109, 139});
    if (shielded) {
        const SDL_FRect outline{bounds.x - 3, bounds.y - 3, bounds.width + 6,
                                bounds.height + 6};
        SDL_SetRenderDrawColor(renderer, 122, 235, 248, 255);
        SDL_RenderRect(renderer, &outline);
    }
    SDL_SetRenderDrawColor(renderer, 222, 236, 245, 255);
    SDL_RenderDebugText(renderer, bounds.x - 5, bounds.y - 12, name.c_str());
}

// Trim drawn over a deck's Renderable fill.
void deckDetails(SDL_Renderer* renderer, Rect bounds, float cameraX,
                 const StarfallSalvage::DeckPalette& palette)
{
    bounds.x -= cameraX;
    rect(renderer, bounds.x, bounds.y, bounds.width, 5, palette.top);
    rect(renderer, bounds.x + 8, bounds.y + 11, bounds.width - 16, 3, palette.line);
    for (float x = bounds.x + 20; x < bounds.x + bounds.width - 10; x += 38) {
        rect(renderer, x, bounds.y + bounds.height - 7, 5, 3, palette.bolt);
    }
}

// Straps drawn over a crate's Renderable fill.
void cargoDetails(SDL_Renderer* renderer, Rect bounds, float cameraX)
{
    bounds.x -= cameraX;
    rect(renderer, bounds.x + 3, bounds.y + 3, bounds.width - 6, bounds.height - 6,
         {255, 184, 76});
    rect(renderer, bounds.x + 10, bounds.y + 3, 4, bounds.height - 6, {148, 70, 39});
    rect(renderer, bounds.x + 3, bounds.y + 10, bounds.width - 6, 4, {148, 70, 39});
}

void drone(SDL_Renderer* renderer, Rect bounds, float cameraX)
{
    bounds.x -= cameraX;
    rect(renderer, bounds.x + 2, bounds.y + 7, bounds.width - 4, 15, {160, 57, 81});
    rect(renderer, bounds.x + 9, bounds.y + 3, bounds.width - 18, 21, {231, 81, 105});
    rect(renderer, bounds.x + 18, bounds.y + 8, 10, 7, {255, 220, 112});
    rect(renderer, bounds.x + 3, bounds.y + 24, 8, 3, {101, 195, 255});
    rect(renderer, bounds.x + bounds.width - 11, bounds.y + 24, 8, 3, {101, 195, 255});
}

int recoveredCargo(std::uint8_t mask)
{
    int count = 0;
    for (std::size_t i = 0; i < StarfallSalvage::cargo.size(); ++i) {
        count += (mask >> i) & 1U;
    }
    return count;
}

} // namespace

StarfallSalvageGame::StarfallSalvageGame(Engine& engine, Multiplayer::Config config)
    : networkTime_(engine.realTime(), kNsPerUs),
      session_(Multiplayer::Session::open(config, engine.realTime(), &networkTime_)),
      config_(std::move(config)),
      station_(StarfallSalvage::buildStation(world_)),
      player_(&StarfallSalvage::makePlayer(world_)),
      astronaut_(player_->get<StarfallSalvage::Astronaut>()),
      sync_(world_, StarfallSalvage::makeRemotePlayer, StarfallSalvage::makeNetworkPlatform),
      workers_({
          [this](float) {
              // Only objects tagged "player", so no other thread writes them.
              if (runPhysics_) {
                  world_.updateComponents(frameTime_, "player");
              }
              ++physicsFrames_;
          },
          [this](float dt) {
              updateSignals(frameJob_, dt);
              ++signalFrames_;
          },
      })
{
    // Nothing to stand on until the session is ready and the station's
    // shuttles and drones have arrived; activated at spawn.
    player_->setActive(false);
    networkTime_.setScale(engine.gameTime().scale());
    engine.setClearColor({7, 14, 31});
}

void StarfallSalvageGame::handleInput(Engine& engine)
{
    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) engine.quit();
    if (Input::isKeyJustPressed(SDL_SCANCODE_F2)) diagnostics_ = !diagnostics_;
    if (Input::isKeyJustPressed(SDL_SCANCODE_P)) engine.gameTime().togglePause();
    if (Input::isKeyJustPressed(SDL_SCANCODE_1)) engine.gameTime().setScale(0.5);
    if (Input::isKeyJustPressed(SDL_SCANCODE_2)) engine.gameTime().setScale(1.0);
    if (Input::isKeyJustPressed(SDL_SCANCODE_3)) engine.gameTime().setScale(2.0);
    if (networkTime_.scale() != engine.gameTime().scale()) {
        networkTime_.setScale(engine.gameTime().scale());
    }
    if (astronaut_->lives == 0 && Input::isKeyJustPressed(SDL_SCANCODE_R)) {
        astronaut_->lives = 3;
        astronaut_->respawn();
    }

    // Engine::run has no built-in frame cap. Pacing here is before its timer
    // tick, so both FrameTime and the actual loop/message rate reflect it.
    // Frames follow a fixed schedule, so the small oversleeps of sleep_for do
    // not add up and slow the loop; a frame that falls more than one interval
    // behind restarts the schedule rather than rushing to catch up.
    const std::int64_t interval = StarfallSalvage::frameIntervalNs(engine.gameTime().scale());
    const std::int64_t now = engine.realTime().now();
    if (nextLoopStartNs_ == 0 || now - nextLoopStartNs_ > interval) {
        nextLoopStartNs_ = now;
    } else if (now < nextLoopStartNs_) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(nextLoopStartNs_ - now));
    }
    nextLoopStartNs_ += interval;

    input_.axis = Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D);
    if (Input::isKeyPressed(SDL_SCANCODE_LEFT)) input_.axis -= 1.0F;
    if (Input::isKeyPressed(SDL_SCANCODE_RIGHT)) input_.axis += 1.0F;
    input_.axis = std::clamp(input_.axis, -1.0F, 1.0F);
    input_.jump = Input::isKeyJustPressed(SDL_SCANCODE_SPACE) ||
                  Input::isKeyJustPressed(SDL_SCANCODE_W) ||
                  Input::isKeyJustPressed(SDL_SCANCODE_UP);
    input_.hold = Input::isKeyPressed(SDL_SCANCODE_E) && !engine.gameTime().isPaused();
}

void StarfallSalvageGame::update(float deltaTime, Engine& engine)
{
    update(FrameTime{static_cast<double>(deltaTime), engine.gameTime().now()}, engine);
}

void StarfallSalvageGame::update(const FrameTime& time, Engine& engine)
{
    currentScale_ = engine.gameTime().scale();
    paused_ = engine.gameTime().isPaused();
    proportionalScale_ = engine.getScaleMode() == Engine::ScaleMode::Proportional;
    gameSeconds_ = static_cast<double>(time.gameTimeUs) / 1'000'000.0;
    loopTics_ = engine.loopTime().now();
    session_->update();
    remote_ = session_->remotePlayers();
    platforms_ = session_->platforms();
    // Remote astronauts, shuttles and drones into the World before anything
    // reads a position this frame.
    sync_.apply(*session_);
    const auto hasPlatform = [this](std::uint32_t id) {
        return std::any_of(platforms_.begin(), platforms_.end(), [id](const auto& platform) {
            return platform.id == id;
        });
    };
    const bool worldComplete = std::all_of(StarfallSalvage::shuttlePaths.begin(),
                                           StarfallSalvage::shuttlePaths.end(),
                                           [&](const auto& path) { return hasPlatform(path.id); }) &&
                               std::all_of(StarfallSalvage::dronePaths.begin(),
                                           StarfallSalvage::dronePaths.end(),
                                           [&](const auto& path) { return hasPlatform(path.id); });
    if (worldComplete) hasWorld_ = true;
    const std::string networkStatus = session_->status();
    if (networkStatus != lastStatus_ || remote_.size() != lastRemoteCount_) {
        std::cout << config_.playerName << ": " << networkStatus << " ("
                  << remote_.size() << " remote player(s))\n";
        lastStatus_ = networkStatus;
        lastRemoteCount_ = remote_.size();
    }

    const bool ready = session_->state() == Multiplayer::State::Ready;
    const bool canMove = ready && hasWorld_ && astronaut_->lives > 0;
    if (ready && hasWorld_ && !spawned_) {
        const auto id = session_->localPlayerId();
        const GameObject& spawn = *station_.spawns[(id ? id - 1 : 0) % station_.spawns.size()];
        astronaut_->spawn = spawn.get<Transform>()->bounds();
        astronaut_->respawn();
        player_->setActive(true);
        spawned_ = true;
    }

    // A closed bulkhead blocks like a deck; opening it only clears its layers.
    station_.bulkhead->get<Collider>()->layers = gateOpen_ ? 0U : StarfallSalvage::solidLayer;

    // The server's shuttles keep moving while this player is paused, so a
    // paused astronaut lets go rather than receive all that travel at once.
    if (paused_ && astronaut_->supportId != 0) {
        astronaut_->detachFromShuttle();
    }
    runPhysics_ = canMove && !paused_ && time.dtSeconds > 0.0;
    if (runPhysics_ && astronaut_->supportId != 0) {
        astronaut_->rideShuttle(sync_.platform(astronaut_->supportId));
    }

    // The workers read these and nothing else; the main thread resumes only
    // after both have finished the frame.
    astronaut_->input = {input_.axis, input_.jump};
    frameTime_ = time;
    frameJob_ = FrameJob{session_->localPlayerId(), remote_};
    workers_.runFrame(static_cast<float>(time.dtSeconds));

    // Back on the main thread: the collision pass pushes the astronaut out of
    // decks, shuttles and the bulkhead, and its callbacks handle landing,
    // drones and death zones. Skipped while it cannot move, so nothing pushes
    // or hurts a paused astronaut.
    Transform& body = *player_->get<Transform>();
    if (runPhysics_) {
        astronaut_->beginCollisionPass();
        world_.resolveCollisions();
        body.x = std::clamp(body.x, 18.0F,
                            StarfallSalvage::worldWidth - 18.0F - StarfallSalvage::playerWidth);
    }
    world_.flushDestroyed();

    if (spawned_) {
        const float target = StarfallSalvage::cameraTargetX(body.x);
        if (!cameraInitialized_ ||
            std::fabs(target - cameraX_) > StarfallSalvage::width * 0.55F) {
            cameraX_ = target;
            cameraInitialized_ = true;
        } else {
            const float blend = 1.0F - std::exp(-10.0F * static_cast<float>(time.dtSeconds));
            cameraX_ += (target - cameraX_) * blend;
        }
    }

    const Rect localBounds = body.bounds();
    const std::uint8_t previousCargoMask = cargoMask_;
    cargoMask_ |= remoteMission_.cargoMask;
    const auto pickedUp = StarfallSalvage::cargoAt(localBounds, cargoMask_, canMove,
                                                   static_cast<float>(time.dtSeconds));
    cargoMask_ |= pickedUp;
    personalCargoMask_ |= pickedUp;
    if (cargoMask_ != previousCargoMask) cargoFlashSeconds_ = 0.75F;
    cargoFlashSeconds_ = std::max(0.0F, cargoFlashSeconds_ -
                                        static_cast<float>(time.dtSeconds));
    int control = -1;
    if (canMove && input_.hold && astronaut_->grounded) {
        control = StarfallSalvage::padUnderPlayer(localBounds);
        if (control < 0) control = StarfallSalvage::gatePlateUnderPlayer(localBounds);
    }
    relays_.occupied = remoteMission_.relays.occupied;
    if (control >= 0 && control < static_cast<int>(StarfallSalvage::relayPads.size())) {
        relays_.occupied[static_cast<std::size_t>(control)] = true;
    }
    const bool someoneCrossing = Collision::intersects(localBounds, StarfallSalvage::bulkhead) ||
        std::any_of(remote_.begin(), remote_.end(), [](const auto& player) {
            return Collision::intersects(
                {player.x, player.y, StarfallSalvage::playerWidth, StarfallSalvage::playerHeight},
                StarfallSalvage::bulkhead);
        });
    gateOpen_ = remoteMission_.gateOpen ||
                control >= static_cast<int>(StarfallSalvage::relayPads.size()) || someoneCrossing;
    relays_.online = relays_.occupied[0] && relays_.occupied[1] && relays_.occupied[2];
    missionComplete_ = cargoMask_ == StarfallSalvage::allCargoMask && relays_.online;
    const std::string localData = StarfallSalvage::encodeState(cargoMask_, control,
                                                            personalCargoMask_);

    // The fixtures show the mission state through their Renderables.
    for (std::size_t i = 0; i < station_.cargo.size(); ++i) {
        station_.cargo[i]->get<Renderable>()->visible = (cargoMask_ & (1U << i)) == 0;
    }
    const float pulseBrightness = 0.5F + 0.5F * std::sin(pulse_ * 6.2831853F);
    for (std::size_t i = 0; i < station_.relayPads.size(); ++i) {
        station_.relayPads[i]->get<Renderable>()->color =
            relays_.occupied[i] ? Color{static_cast<std::uint8_t>(85 + 30 * pulseBrightness), 238, 209}
                                : Color{70, 104, 134};
    }
    for (GameObject* plate : station_.gatePlates) {
        plate->get<Renderable>()->color = gateOpen_ ? Color{81, 224, 186} : Color{247, 180, 93};
    }

    std::vector<StarfallSalvage::Claim> claims;
    claims.reserve(remote_.size() + 1);
    for (const auto& player : remote_) {
        claims.push_back({player.id, player.x, player.y, player.data});
    }
    if (ready && spawned_) {
        claims.push_back({session_->localPlayerId(), localBounds.x, localBounds.y, localData});
    }
    creditedCargoMask_ = StarfallSalvage::creditedCargoMask(claims, session_->localPlayerId());

    cargoWinner_ = {};
    winnerName_.clear();
    if (missionComplete_) {
        cargoWinner_ = StarfallSalvage::chooseCargoWinner(claims);
        if (cargoWinner_.playerId == session_->localPlayerId()) {
            winnerName_ = config_.playerName;
        } else {
            const auto winner = std::find_if(remote_.begin(), remote_.end(), [this](const auto& player) {
                return player.id == cargoWinner_.playerId;
            });
            if (winner != remote_.end()) winnerName_ = winner->name;
        }
        if (winnerName_.empty()) {
            winnerName_ = "PLAYER " + std::to_string(cargoWinner_.playerId);
        }
    }

    if (ready && hasWorld_ && spawned_) {
        session_->publishLocalPlayer(localBounds.x, localBounds.y, localData);
        ++ratePublishCount_;
    }

    const auto now = engine.realTime().now();
    if (rateSampleStartNs_ == 0) rateSampleStartNs_ = now;
    ++rateLoopCount_;
    const double elapsed = static_cast<double>(now - rateSampleStartNs_) / 1'000'000'000.0;
    if (elapsed >= 1.0) {
        measuredLoopHz_ = static_cast<double>(rateLoopCount_) / elapsed;
        publishCallsHz_ = static_cast<double>(ratePublishCount_) / elapsed;
        rateLoopCount_ = 0;
        ratePublishCount_ = 0;
        rateSampleStartNs_ = now;
    }
}

void StarfallSalvageGame::updateSignals(const FrameJob& job, float deltaTime)
{
    std::vector<StarfallSalvage::Claim> claims;
    claims.reserve(job.remote.size());
    for (const auto& player : job.remote) {
        if (player.id != job.localId) {
            claims.push_back({player.id, player.x, player.y, player.data});
        }
    }
    remoteMission_ = StarfallSalvage::evaluateMission(claims);
    pulse_ = std::fmod(pulse_ + deltaTime * 0.55F, 1.0F);
}

std::string StarfallSalvageGame::remoteName(std::uint32_t id) const
{
    const auto player = std::find_if(remote_.begin(), remote_.end(),
                                     [id](const auto& entry) { return entry.id == id; });
    return player == remote_.end() || player->name.empty() ? std::string("PEER") : player->name;
}

void StarfallSalvageGame::render(SDL_Renderer* renderer) const
{
    // Everything below the HUD is drawn from world coordinates through cameraX_.
    worldRect(renderer, cameraX_, 16, 76, StarfallSalvage::worldWidth - 32, 418,
              {13, 26, 47});
    worldRect(renderer, cameraX_, 27, 88, StarfallSalvage::worldWidth - 54, 399,
              {10, 21, 40});
    for (int i = 0; i < 80; ++i) {
        const float x = static_cast<float>((i * 187 + 63) %
                                           (StarfallSalvage::worldWidth - 40) + 20);
        const float y = static_cast<float>((i * 119 + 87) % 390 + 83);
        const float size = i % 7 == 0 ? 3.0F : 1.5F;
        worldRect(renderer, cameraX_, x, y, size, size, {104, 150, 196});
    }
    worldRect(renderer, cameraX_, 27, 91, StarfallSalvage::worldWidth - 54, 8,
              {39, 68, 101});
    worldRect(renderer, cameraX_, 27, 95, StarfallSalvage::worldWidth - 54, 2,
              {108, 164, 203});
    for (float x : {95.0F, 338.0F, 583.0F, 838.0F, 1120.0F, 1410.0F}) {
        worldRect(renderer, cameraX_, x, 99, 10, 398, {28, 50, 79});
        worldRect(renderer, cameraX_, x + 2, 99, 2, 398, {55, 90, 130});
    }
    SDL_SetRenderDrawColor(renderer, 129, 191, 218, 255);
    SDL_RenderDebugText(renderer, 885.0F - cameraX_, 105.0F, "EAST WING >>");
    SDL_RenderDebugText(renderer, 1300.0F - cameraX_, 105.0F, "RELAY ARRAY");

    // Rails show where the server-authored shuttles and drones travel.
    for (const auto& path : StarfallSalvage::shuttlePaths) {
        if (path.startY == path.endY) {
            worldRect(renderer, cameraX_, path.startX, path.startY + 22,
                      path.endX - path.startX + path.width, 3, {49, 85, 121});
            worldRect(renderer, cameraX_, path.startX, path.startY + 25, 4, 7,
                      {80, 136, 171});
            worldRect(renderer, cameraX_, path.endX + path.width - 4,
                      path.endY + 25, 4, 7, {80, 136, 171});
        } else {
            // A lift: a vertical rail with a stop at the bottom.
            const float top = std::min(path.startY, path.endY);
            const float bottom = std::max(path.startY, path.endY) + path.height;
            worldRect(renderer, cameraX_, path.startX + path.width * 0.5F - 1.5F, top,
                      3, bottom - top + 12, {49, 85, 121});
            worldRect(renderer, cameraX_, path.startX + 10, bottom + 10, path.width - 20, 4,
                      {80, 136, 171});
        }
    }
    for (const auto& path : StarfallSalvage::dronePaths) {
        if (path.shape == Network::PlatformPath::Shape::Circular) {
            worldRing(renderer, cameraX_, path.centerX + path.width * 0.5F,
                      path.centerY + path.height * 0.5F, path.radius, {126, 53, 70});
            continue;
        }
        worldRect(renderer, cameraX_, path.startX, path.startY + 29,
                  path.endX - path.startX + path.width, 2, {126, 53, 70});
        worldRect(renderer, cameraX_, path.startX, path.startY + 27, 4, 6,
                  {231, 107, 102});
        worldRect(renderer, cameraX_, path.endX + path.width - 4,
                  path.endY + 27, 4, 6, {231, 107, 102});
    }

    // Housings behind the pads and switches, which renderWorld draws on top.
    for (std::size_t i = 0; i < StarfallSalvage::relayPads.size(); ++i) {
        const Rect& pad = StarfallSalvage::relayPads[i];
        worldRect(renderer, cameraX_, pad.x - 8, pad.y - 28, pad.width + 16, 29, {23, 45, 68});
        worldRect(renderer, cameraX_, pad.x - 3, pad.y - 24, pad.width + 6, 18,
                  relays_.occupied[i] ? Color{38, 128, 120} : Color{40, 71, 97});
    }
    for (const Rect& plate : StarfallSalvage::gatePlates) {
        worldRect(renderer, cameraX_, plate.x - 5, plate.y - 13, plate.width + 10, 14, {26, 44, 64});
    }

    // Every deck, shuttle, crate, pad and switch: their Renderable fills.
    renderWorld(renderer, world_, cameraX_, 0.0F);

    // Detail drawn over those fills, and the drones, which are sprites.
    for (const auto& object : world_.objects()) {
        if (!object->isActive()) continue;
        const Rect bounds = object->get<Transform>()->bounds();
        if (const auto* style = object->get<StarfallSalvage::DeckStyle>()) {
            deckDetails(renderer, bounds, cameraX_, StarfallSalvage::paletteFor(style->kind));
        } else if (object->tag() == "shuttle") {
            deckDetails(renderer, bounds, cameraX_, StarfallSalvage::paletteFor(StarfallSalvage::DeckKind::Hull));
            worldRect(renderer, cameraX_, bounds.x + 8, bounds.y + 7, 8, 4, {91, 232, 241});
            worldRect(renderer, cameraX_, bounds.x + bounds.width - 16, bounds.y + 7, 8, 4,
                      {91, 232, 241});
        } else if (object->tag() == "cargo" && object->get<Renderable>()->visible) {
            cargoDetails(renderer, bounds, cameraX_);
        } else if (object->tag() == "drone") {
            drone(renderer, bounds, cameraX_);
        }
    }

    // The death zone itself is hidden, but the gap it sits in must be easy to
    // see: most of the floor is under the HUD bar, so the breach gets hazard
    // stripes on both edges, a red glow and a sign on the wall above.
    const Rect& breach = StarfallSalvage::hullBreach;
    const float breachPulse = 0.5F + 0.5F * std::sin(pulse_ * 6.2831853F * 2.0F);
    for (int i = 0; i < 4; ++i) {
        const Color stripe = i % 2 == 0 ? Color{247, 201, 72} : Color{30, 30, 34};
        const float offset = 6.0F * static_cast<float>(i);
        worldRect(renderer, cameraX_, breach.x - 24.0F + offset, breach.y, 6, 7, stripe);
        worldRect(renderer, cameraX_, breach.x + breach.width + offset, breach.y, 6, 7, stripe);
    }
    worldRect(renderer, cameraX_, breach.x, breach.y, breach.width, 7,
              {static_cast<std::uint8_t>(120 + 100 * breachPulse), 30, 36});
    // The sign is 11 characters of 8 px, centred over the gap.
    const float signX = breach.x + breach.width * 0.5F - 44.0F;
    worldRect(renderer, cameraX_, signX - 4.0F, 436, 96, 15, {58, 18, 26});
    SDL_SetRenderDrawColor(renderer, 255, static_cast<std::uint8_t>(120 + 80 * breachPulse), 110, 255);
    SDL_RenderDebugText(renderer, signX - cameraX_, 440, "HULL BREACH");

    for (std::size_t i = 0; i < StarfallSalvage::gatePlates.size(); ++i) {
        const Rect& plate = StarfallSalvage::gatePlates[i];
        SDL_SetRenderDrawColor(renderer, 238, 230, 210, 255);
        SDL_RenderDebugText(renderer, plate.x - cameraX_ - 12, plate.y - 27,
                            i == 0 ? "HOLD E: GATE" : "HOLD E: RETURN");
    }
    if (gateOpen_) {
        worldRect(renderer, cameraX_, StarfallSalvage::bulkhead.x, 99, 3, 401,
                  {72, 195, 174});
        worldRect(renderer, cameraX_, StarfallSalvage::bulkhead.x + 17, 99, 3, 401,
                  {72, 195, 174});
    } else {
        worldRect(renderer, cameraX_, StarfallSalvage::bulkhead.x, 99,
                  StarfallSalvage::bulkhead.width, 401, {101, 43, 58});
        for (float y = 112.0F; y < 490.0F; y += 24.0F) {
            worldRect(renderer, cameraX_, StarfallSalvage::bulkhead.x + 3, y, 14, 6,
                      {239, 108, 104});
        }
    }

    const Rect localBounds = player_->get<Transform>()->bounds();
    if (!missionComplete_) {
        rect(renderer, 306, 110, 348, 27, {18, 43, 66});
        SDL_SetRenderDrawColor(renderer, 255, 213, 139, 255);
        const bool nearBulkhead = spawned_ && localBounds.x > 720.0F &&
                                  localBounds.x < StarfallSalvage::bulkhead.x &&
                                  cargoMask_ != StarfallSalvage::allCargoMask;
        const char* objective = cargoFlashSeconds_ > 0.0F ? "CARGO SECURED" :
                                cargoMask_ == StarfallSalvage::allCargoMask
                                    ? "HOLD E ON THREE RELAYS TOGETHER" :
                                nearBulkhead ? (gateOpen_ ? "GATE OPEN: CROSS TO EAST WING"
                                                          : "HOLD E AT SWITCH; PARTNER CROSSES")
                                             : "TOUCH THE FOUR ORANGE CARGO CRATES";
        SDL_RenderDebugText(renderer, cargoFlashSeconds_ > 0.0F ? 403.0F : 319.0F,
                            119.0F, objective);
    }

    for (std::size_t i = 0; i < StarfallSalvage::relayPads.size(); ++i) {
        const Rect& pad = StarfallSalvage::relayPads[i];
        const bool active = relays_.occupied[i];
        worldRect(renderer, cameraX_, pad.x + pad.width * 0.5F - 4, pad.y - 18, 8, 8,
                  active ? Color{218, 251, 214} : Color{77, 118, 149});
        SDL_SetRenderDrawColor(renderer, 233, 243, 250, 255);
        SDL_RenderDebugTextFormat(renderer, pad.x - cameraX_ + 10, pad.y - 39, "RELAY %zu", i + 1);
    }

    const float pulseBrightness = 0.5F + 0.5F * std::sin(pulse_ * 6.2831853F);
    if (relays_.online) {
        worldLine(renderer, cameraX_, 150, 389, 580, 309, {85, 224, 202});
        worldLine(renderer, cameraX_, 580, 309, 1415, 229, {85, 224, 202});
        worldLine(renderer, cameraX_, 150, 387, 580, 307, {202, 255, 229});
        worldLine(renderer, cameraX_, 580, 307, 1415, 227, {202, 255, 229});
        rect(renderer, 279, 128, 402, 59, {16, 69, 77});
        rect(renderer, 284, 133, 392, 49,
             {static_cast<std::uint8_t>(40 + 20 * pulseBrightness), 122, 117});
        SDL_SetRenderDrawColor(renderer, 230, 255, 229, 255);
        if (missionComplete_) {
            SDL_RenderDebugText(renderer, 374, 143, "SALVAGE COMPLETE");
            const std::string winnerText = "WINNER: " + winnerName_ + " (" +
                std::to_string(cargoWinner_.crates) + " CARGO)";
            SDL_RenderDebugText(renderer, 318, 163, winnerText.c_str());
        } else {
            SDL_RenderDebugText(renderer, 355, 152, "RECOVER THE CARGO");
        }
    }

    // Astronauts are multi-part sprites drawn at their objects' Transforms.
    for (const auto& object : world_.objects()) {
        if (!object->isActive() || object->tag() != "remote") continue;
        const std::uint32_t id = object->get<NetworkIdentity>()->id;
        astronaut(renderer, object->get<Transform>()->bounds(), suitFor(id), remoteName(id), cameraX_);
    }
    if (spawned_) {
        const float stride = astronaut_->grounded && !paused_ && input_.axis != 0.0F
                                 ? static_cast<float>(std::sin(gameSeconds_ * 16.0) * 2.5)
                                 : 0.0F;
        astronaut(renderer, localBounds, suitFor(session_->localPlayerId()),
                  config_.playerName + " (YOU)", cameraX_, stride, !astronaut_->grounded,
                  astronaut_->invulnerableSeconds > 0.0F);
    }

    rect(renderer, 0, 0, 960, 68, {8, 18, 35});
    rect(renderer, 0, 66, 960, 3, {45, 92, 128});
    SDL_SetRenderDrawColor(renderer, 130, 231, 237, 255);
    SDL_RenderDebugText(renderer, 20, 11, "S T A R F A L L   S A L V A G E");
    SDL_SetRenderDrawColor(renderer, gateOpen_ ? 114 : 247,
                           gateOpen_ ? 230 : 168, gateOpen_ ? 188 : 112, 255);
    SDL_RenderDebugText(renderer, 830, 11, gateOpen_ ? "GATE OPEN" : "GATE CLOSED");
    SDL_SetRenderDrawColor(renderer, 230, 236, 245, 255);
    const int relayCount = static_cast<int>(relays_.occupied[0]) +
                           static_cast<int>(relays_.occupied[1]) +
                           static_cast<int>(relays_.occupied[2]);
    const std::size_t playerCount = remote_.size() +
        (session_->state() == Multiplayer::State::Ready ? 1U : 0U);
    SDL_RenderDebugTextFormat(renderer, 20, 32,
                              "CARGO %d/4 | LIVES %d | RELAYS %d/3 | %s | %zu PLAYERS | %.1fX | %.1f LOOP/S | %s | %s",
                              recoveredCargo(cargoMask_), astronaut_->lives, relayCount,
                              Multiplayer::modeName(session_->mode()), playerCount,
                              currentScale_, measuredLoopHz_,
                              proportionalScale_ ? "FIT" : "PIXEL",
                              paused_ ? "PAUSED" : "LIVE");
    SDL_SetRenderDrawColor(renderer, 255, 203, 127, 255);
    const std::string status = session_->status();
    SDL_RenderDebugText(renderer, 20, 48, status.substr(0, 111).c_str());

    rect(renderer, 0, 507, 960, 33, {8, 18, 35});
    SDL_SetRenderDrawColor(renderer, 175, 196, 220, 255);
    SDL_RenderDebugText(renderer, 18, 516,
                        "A/D MOVE   SPACE DOUBLE-JUMP   E RELAY   P PAUSE   1/2/3 SPEED   F1 SCALE   F2 INFO");
    SDL_SetRenderDrawColor(renderer, 255, 203, 127, 255);
    SDL_RenderDebugTextFormat(renderer, 824, 516, "YOU %d CARGO",
                              recoveredCargo(creditedCargoMask_));

    if (astronaut_->lives == 0 && session_->state() == Multiplayer::State::Ready) {
        rect(renderer, 285, 204, 390, 78, {83, 27, 45});
        SDL_SetRenderDrawColor(renderer, 255, 225, 228, 255);
        SDL_RenderDebugText(renderer, 411, 225, "SUIT DISABLED");
        SDL_RenderDebugText(renderer, 374, 248, "PRESS R TO RESPAWN");
    }

    if (!hasWorld_ || session_->state() != Multiplayer::State::Ready) {
        rect(renderer, 245, 208, 470, 75, {11, 31, 54});
        SDL_SetRenderDrawColor(renderer, 233, 239, 245, 255);
        SDL_RenderDebugText(renderer, 321, 231,
            session_->state() == Multiplayer::State::Failed ? "CONNECTION ERROR" :
            !hasWorld_ ? "WAITING FOR STATION WORLD" : "CONNECTING TO SERVER");
        SDL_RenderDebugText(renderer, 310, 252, "WINDOW AND NETWORK STAY RESPONSIVE");
    }

    if (diagnostics_) {
        rect(renderer, 17, 79, 344, 108, {9, 26, 47});
        SDL_SetRenderDrawColor(renderer, 182, 226, 237, 255);
        SDL_RenderDebugTextFormat(renderer, 25, 87, "WORKERS PHYSICS %llu / SIGNAL %llu",
            static_cast<unsigned long long>(physicsFrames_),
            static_cast<unsigned long long>(signalFrames_));
        SDL_RenderDebugTextFormat(renderer, 25, 104, "PUBLISH CALLS/S %.1f (NOT WIRE)", publishCallsHz_);
        SDL_RenderDebugTextFormat(renderer, 25, 121, "WORLD %s | TIME %s",
            session_->authorityState() == Multiplayer::AuthorityState::Ready ? "LIVE" : "STALE/WAIT",
            paused_ ? "PAUSED" : "RUNNING");
        SDL_RenderDebugTextFormat(renderer, 25, 138, "GAME %.2f S | LOOP %lld",
            gameSeconds_, static_cast<long long>(loopTics_));
        SDL_RenderDebugTextFormat(renderer, 25, 155, "RENDER %s",
            proportionalScale_ ? "PROPORTIONAL" : "CONSTANT");
        SDL_RenderDebugTextFormat(renderer, 25, 172, "CAMERA %.0f / %d",
            cameraX_, StarfallSalvage::worldWidth - StarfallSalvage::width);
    }
}
