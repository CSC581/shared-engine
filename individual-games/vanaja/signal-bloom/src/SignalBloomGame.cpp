#include "SignalBloomGame.hpp"

#include "Collision.hpp"
#include "Engine.hpp"
#include "Input.hpp"
#include "Physics.hpp"
#include "SignalBloomWorld.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

struct Color {
    Uint8 r;
    Uint8 g;
    Uint8 b;
};

constexpr std::array<Color, 8> suits{{
    {105, 225, 245}, {255, 185, 95}, {245, 115, 155}, {154, 224, 148},
    {180, 161, 255}, {245, 210, 110}, {105, 170, 255}, {235, 150, 210},
}};

void rect(SDL_Renderer* renderer, float x, float y, float w, float h, Color color)
{
    const SDL_FRect box{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    SDL_RenderFillRect(renderer, &box);
}

void rect(SDL_Renderer* renderer, Rect box, Color color)
{
    rect(renderer, box.x, box.y, box.width, box.height, color);
}

void line(SDL_Renderer* renderer, float x1, float y1, float x2, float y2, Color color)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    SDL_RenderLine(renderer, x1, y1, x2, y2);
}

Color suitFor(std::uint32_t id)
{
    return suits[(id ? id - 1 : 0) % suits.size()];
}

void astronaut(SDL_Renderer* renderer, Rect bounds, Color suit, const std::string& name)
{
    rect(renderer, bounds.x + 2, bounds.y + 13, bounds.width - 4, 24, {17, 41, 61});
    rect(renderer, bounds.x + 4, bounds.y + 15, bounds.width - 8, 20, suit);
    rect(renderer, bounds.x + 1, bounds.y + 3, bounds.width - 2, 19, {207, 238, 244});
    rect(renderer, bounds.x + 4, bounds.y + 6, bounds.width - 8, 11, {28, 82, 111});
    rect(renderer, bounds.x + 7, bounds.y + 8, 6, 2, {136, 233, 250});
    rect(renderer, bounds.x + 2, bounds.y + 35, 10, 5, {63, 109, 139});
    rect(renderer, bounds.x + bounds.width - 12, bounds.y + 35, 10, 5, {63, 109, 139});
    SDL_SetRenderDrawColor(renderer, 222, 236, 245, 255);
    SDL_RenderDebugText(renderer, bounds.x - 5, bounds.y - 12, name.c_str());
}

void deck(SDL_Renderer* renderer, Rect bounds)
{
    rect(renderer, bounds, {37, 58, 86});
    rect(renderer, bounds.x, bounds.y, bounds.width, 5, {115, 174, 208});
    rect(renderer, bounds.x + 8, bounds.y + 11, bounds.width - 16, 3, {65, 100, 137});
    for (float x = bounds.x + 20; x < bounds.x + bounds.width - 10; x += 38) {
        rect(renderer, x, bounds.y + bounds.height - 7, 5, 3, {80, 118, 151});
    }
}

} // namespace

SignalBloomGame::SignalBloomGame(Engine& engine, Multiplayer::Config config)
    : session_(Multiplayer::Session::open(config, engine.realTime())), config_(std::move(config))
{
    Physics::setGravity(SignalBloom::gravity);
    engine.setClearColor({7, 14, 31});
    physicsThread_ = std::thread([this] { physicsLoop(); });
    try {
        signalThread_ = std::thread([this] { signalLoop(); });
    } catch (...) {
        {
            const std::lock_guard<std::mutex> lock(frameMutex_);
            stopping_ = true;
        }
        frameCv_.notify_all();
        physicsThread_.join();
        throw;
    }
}

SignalBloomGame::~SignalBloomGame()
{
    {
        const std::lock_guard<std::mutex> lock(frameMutex_);
        stopping_ = true;
    }
    frameCv_.notify_all();
    if (physicsThread_.joinable()) physicsThread_.join();
    if (signalThread_.joinable()) signalThread_.join();
    session_.reset();
}

void SignalBloomGame::handleInput(Engine& engine)
{
    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) engine.quit();
    if (Input::isKeyJustPressed(SDL_SCANCODE_F2)) diagnostics_ = !diagnostics_;
    if (Input::isKeyJustPressed(SDL_SCANCODE_P)) engine.gameTime().togglePause();
    if (Input::isKeyJustPressed(SDL_SCANCODE_1)) engine.gameTime().setScale(0.5);
    if (Input::isKeyJustPressed(SDL_SCANCODE_2)) engine.gameTime().setScale(1.0);
    if (Input::isKeyJustPressed(SDL_SCANCODE_3)) engine.gameTime().setScale(2.0);

    // Engine::run has no built-in frame cap. Pacing here is before its timer
    // tick, so both FrameTime and the actual loop/message rate reflect it.
    const std::int64_t interval = SignalBloom::frameIntervalNs(engine.gameTime().scale());
    const std::int64_t now = engine.realTime().now();
    if (lastLoopStartNs_ != 0 && now < lastLoopStartNs_ + interval) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(lastLoopStartNs_ + interval - now));
    }
    lastLoopStartNs_ = engine.realTime().now();

    input_.axis = Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D);
    if (Input::isKeyPressed(SDL_SCANCODE_LEFT)) input_.axis -= 1.0F;
    if (Input::isKeyPressed(SDL_SCANCODE_RIGHT)) input_.axis += 1.0F;
    input_.axis = std::clamp(input_.axis, -1.0F, 1.0F);
    input_.jump = Input::isKeyJustPressed(SDL_SCANCODE_SPACE) ||
                  Input::isKeyJustPressed(SDL_SCANCODE_W) ||
                  Input::isKeyJustPressed(SDL_SCANCODE_UP);
    input_.hold = Input::isKeyPressed(SDL_SCANCODE_E) && !engine.gameTime().isPaused();
}

void SignalBloomGame::update(float deltaTime, Engine& engine)
{
    update(FrameTime{static_cast<double>(deltaTime), engine.gameTime().now()}, engine);
}

void SignalBloomGame::update(const FrameTime& time, Engine& engine)
{
    currentScale_ = engine.gameTime().scale();
    paused_ = engine.gameTime().isPaused();
    gameSeconds_ = static_cast<double>(time.gameTimeUs) / 1'000'000.0;
    loopTics_ = engine.loopTime().now();
    session_->update();
    remote_ = session_->remotePlayers();
    platforms_ = session_->platforms();
    if (platforms_.size() == 2) hasWorld_ = true;
    const std::string networkStatus = session_->status();
    if (networkStatus != lastStatus_ || remote_.size() != lastRemoteCount_) {
        std::cout << config_.playerName << ": " << networkStatus << " ("
                  << remote_.size() << " remote player(s))\n";
        lastStatus_ = networkStatus;
        lastRemoteCount_ = remote_.size();
    }

    const bool ready = session_->state() == Multiplayer::State::Ready;
    const bool canMove = ready && hasWorld_;
    if (canMove && !spawned_) {
        const auto id = session_->localPlayerId();
        const auto& spawn = SignalBloom::spawns[(id ? id - 1 : 0) % SignalBloom::spawns.size()];
        player_.setPosition(spawn.x, spawn.y);
        spawned_ = true;
    }

    {
        std::unique_lock<std::mutex> lock(frameMutex_);
        frameJob_ = FrameJob{time, input_, canMove, session_->localPlayerId(), platforms_, remote_};
        workersDone_ = 0;
        ++generation_;
        frameCv_.notify_all();
        frameCv_.wait(lock, [this] { return stopping_ || workersDone_ == 2; });
    }

    const Rect localBounds = player_.getBounds();
    const int pad = canMove && input_.hold && grounded_
                        ? SignalBloom::padUnderPlayer(localBounds) : -1;
    relays_.occupied = remoteOccupied_;
    if (pad >= 0) relays_.occupied[static_cast<std::size_t>(pad)] = true;
    relays_.online = relays_.occupied[0] && relays_.occupied[1] && relays_.occupied[2];

    if (canMove) {
        session_->publishLocalPlayer(localBounds.x, localBounds.y, SignalBloom::encodeClaim(pad));
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

void SignalBloomGame::physicsLoop()
{
    std::uint64_t seen = 0;
    while (true) {
        FrameJob job;
        {
            std::unique_lock<std::mutex> lock(frameMutex_);
            frameCv_.wait(lock, [this, seen] { return stopping_ || generation_ != seen; });
            if (stopping_) return;
            seen = generation_;
            job = frameJob_;
        }
        simulatePlayer(job);
        ++physicsFrames_;
        workerDone();
    }
}

void SignalBloomGame::signalLoop()
{
    std::uint64_t seen = 0;
    while (true) {
        FrameJob job;
        {
            std::unique_lock<std::mutex> lock(frameMutex_);
            frameCv_.wait(lock, [this, seen] { return stopping_ || generation_ != seen; });
            if (stopping_) return;
            seen = generation_;
            job = frameJob_;
        }

        std::vector<SignalBloom::Claim> claims;
        claims.reserve(job.remote.size());
        for (const auto& player : job.remote) {
            if (player.id != job.localId) {
                claims.push_back({player.id, player.x, player.y, player.data});
            }
        }
        remoteOccupied_ = SignalBloom::evaluateRelays(claims).occupied;
        pulse_ = std::fmod(pulse_ + static_cast<float>(job.time.dtSeconds) * 0.55F, 1.0F);
        ++signalFrames_;
        workerDone();
    }
}

void SignalBloomGame::workerDone()
{
    const std::lock_guard<std::mutex> lock(frameMutex_);
    ++workersDone_;
    if (workersDone_ == 2) frameCv_.notify_all();
}

void SignalBloomGame::simulatePlayer(const FrameJob& job)
{
    if (supportId_ != 0 && job.canMove && job.time.dtSeconds > 0.0) {
        const auto oldPlatform = std::find_if(lastPhysicsPlatforms_.begin(), lastPhysicsPlatforms_.end(),
            [this](const auto& platform) { return platform.id == supportId_; });
        const auto newPlatform = std::find_if(job.platforms.begin(), job.platforms.end(),
            [this](const auto& platform) { return platform.id == supportId_; });
        if (oldPlatform != lastPhysicsPlatforms_.end() && newPlatform != job.platforms.end()) {
            const float dx = newPlatform->x - oldPlatform->x;
            const float dy = newPlatform->y - oldPlatform->y;
            if (std::fabs(dx) <= 32.0F && std::fabs(dy) <= 32.0F) {
                player_.setPosition(player_.getX() + dx, player_.getY() + dy);
            } else {
                supportId_ = 0;
                grounded_ = false;
            }
        } else {
            supportId_ = 0;
            grounded_ = false;
        }
    }
    lastPhysicsPlatforms_ = job.platforms;

    if (!job.canMove || job.time.dtSeconds <= 0.0) return;

    if (job.input.jump && grounded_) {
        player_.setVelocityY(-SignalBloom::jumpSpeed);
        grounded_ = false;
        supportId_ = 0;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(job.time.dtSeconds / 0.01)));
    const float step = static_cast<float>(job.time.dtSeconds / steps);
    for (int i = 0; i < steps; ++i) {
        const Rect previous = player_.getBounds();
        player_.setVelocityX(job.input.axis * SignalBloom::playerSpeed);
        Physics::applyGravity(player_, step);
        player_.update(step);
        grounded_ = false;
        supportId_ = 0;

        for (const Rect& box : SignalBloom::decks) resolveBlocker(box, 0, previous);
        for (const auto& platform : job.platforms) {
            resolveBlocker({platform.x, platform.y, platform.width, platform.height},
                           platform.id, previous);
        }
        const float x = std::clamp(player_.getX(), 18.0F,
                                   SignalBloom::width - 18.0F - SignalBloom::playerWidth);
        player_.setPosition(x, player_.getY());
        if (player_.getY() > SignalBloom::height + 60.0F) respawn();
    }
}

void SignalBloomGame::resolveBlocker(const Rect& blocker, std::uint32_t platformId,
                                     const Rect& previous)
{
    Rect player = player_.getBounds();
    if (!Collision::intersects(player, blocker)) return;

    const float previousBottom = previous.y + previous.height;
    const float currentBottom = player.y + player.height;
    if (previousBottom <= blocker.y + 5.0F && player_.getVelocityY() >= 0.0F &&
        currentBottom >= blocker.y) {
        player_.setPosition(player.x, blocker.y - player.height);
        player_.setVelocityY(0.0F);
        grounded_ = true;
        supportId_ = platformId;
        return;
    }
    if (previous.y >= blocker.y + blocker.height - 5.0F && player_.getVelocityY() < 0.0F) {
        player_.setPosition(player.x, blocker.y + blocker.height);
        player_.setVelocityY(0.0F);
        return;
    }

    const Rect overlap = Collision::getIntersection(player, blocker);
    if (overlap.width < overlap.height) {
        const float direction = player.x + player.width * 0.5F <
                                        blocker.x + blocker.width * 0.5F ? -1.0F : 1.0F;
        player_.setPosition(player.x + direction * overlap.width, player.y);
        player_.setVelocityX(0.0F);
    } else {
        const bool above = player.y + player.height * 0.5F <
                           blocker.y + blocker.height * 0.5F;
        player_.setPosition(player.x, player.y + (above ? -overlap.height : overlap.height));
        player_.setVelocityY(0.0F);
        if (above) {
            grounded_ = true;
            supportId_ = platformId;
        }
    }
}

void SignalBloomGame::respawn()
{
    player_.setPosition(SignalBloom::spawns[0].x, SignalBloom::spawns[0].y);
    player_.setVelocity(0.0F, 0.0F);
    grounded_ = false;
    supportId_ = 0;
}

void SignalBloomGame::render(SDL_Renderer* renderer) const
{
    // Far station: framed starfield and restrained parallax-like light bands.
    rect(renderer, 16, 76, 928, 418, {13, 26, 47});
    rect(renderer, 27, 88, 906, 399, {10, 21, 40});
    for (int i = 0; i < 47; ++i) {
        const float x = static_cast<float>((i * 187 + 63) % 920 + 20);
        const float y = static_cast<float>((i * 119 + 87) % 390 + 83);
        const float size = i % 7 == 0 ? 3.0F : 1.5F;
        rect(renderer, x, y, size, size, {104, 150, 196});
    }
    rect(renderer, 27, 91, 906, 8, {39, 68, 101});
    rect(renderer, 27, 95, 906, 2, {108, 164, 203});
    for (float x : {95.0F, 338.0F, 583.0F, 838.0F}) {
        rect(renderer, x, 99, 10, 398, {28, 50, 79});
        rect(renderer, x + 2, 99, 2, 398, {55, 90, 130});
    }

    // Rails show where server-authored shuttles may travel.
    for (const auto& path : SignalBloom::shuttlePaths) {
        rect(renderer, path.startX, path.startY + 22, path.endX - path.startX + path.width,
             3, {49, 85, 121});
        rect(renderer, path.startX, path.startY + 25, 4, 7, {80, 136, 171});
        rect(renderer, path.endX + path.width - 4, path.endY + 25, 4, 7, {80, 136, 171});
    }
    for (const Rect& box : SignalBloom::decks) deck(renderer, box);
    for (const auto& platform : platforms_) {
        deck(renderer, {platform.x, platform.y, platform.width, platform.height});
        rect(renderer, platform.x + 8, platform.y + 7, 8, 4, {91, 232, 241});
        rect(renderer, platform.x + platform.width - 16, platform.y + 7, 8, 4,
             {91, 232, 241});
    }

    const Color dark{70, 104, 134};
    const float pulseBrightness = 0.5F + 0.5F * std::sin(pulse_ * 6.2831853F);
    for (std::size_t i = 0; i < SignalBloom::relayPads.size(); ++i) {
        const Rect pad = SignalBloom::relayPads[i];
        const bool active = relays_.occupied[i];
        rect(renderer, pad.x - 8, pad.y - 28, pad.width + 16, 29, {23, 45, 68});
        rect(renderer, pad.x - 3, pad.y - 24, pad.width + 6, 18,
             active ? Color{38, 128, 120} : Color{40, 71, 97});
        const Color animated = active
            ? Color{static_cast<Uint8>(85 + 30 * pulseBrightness), 238, 209} : dark;
        rect(renderer, pad, animated);
        rect(renderer, pad.x + pad.width * 0.5F - 4, pad.y - 18, 8, 8,
             active ? Color{218, 251, 214} : Color{77, 118, 149});
        SDL_SetRenderDrawColor(renderer, 233, 243, 250, 255);
        SDL_RenderDebugTextFormat(renderer, pad.x + 10, pad.y - 39, "RELAY %zu", i + 1);
    }

    if (relays_.online) {
        line(renderer, 149, 389, 580, 309, {85, 224, 202});
        line(renderer, 580, 309, 877, 229, {85, 224, 202});
        line(renderer, 149, 387, 580, 307, {202, 255, 229});
        line(renderer, 580, 307, 877, 227, {202, 255, 229});
        rect(renderer, 279, 128, 402, 59, {16, 69, 77});
        rect(renderer, 284, 133, 392, 49,
             {static_cast<Uint8>(40 + 20 * pulseBrightness), 122, 117});
        SDL_SetRenderDrawColor(renderer, 230, 255, 229, 255);
        SDL_RenderDebugText(renderer, 385, 152, "STATION ONLINE");
    }

    for (const auto& player : remote_) {
        astronaut(renderer, {player.x, player.y, SignalBloom::playerWidth, SignalBloom::playerHeight},
                  suitFor(player.id), player.name.empty() ? "PEER" : player.name);
    }
    if (spawned_) {
        astronaut(renderer, player_.getBounds(), suitFor(session_->localPlayerId()),
                  config_.playerName + " (YOU)");
    }

    rect(renderer, 0, 0, 960, 68, {8, 18, 35});
    rect(renderer, 0, 66, 960, 3, {45, 92, 128});
    SDL_SetRenderDrawColor(renderer, 130, 231, 237, 255);
    SDL_RenderDebugText(renderer, 20, 11, "S I G N A L   B L O O M");
    SDL_SetRenderDrawColor(renderer, 230, 236, 245, 255);
    const int relayCount = static_cast<int>(relays_.occupied[0]) +
                           static_cast<int>(relays_.occupied[1]) +
                           static_cast<int>(relays_.occupied[2]);
    const std::size_t playerCount = remote_.size() +
        (session_->state() == Multiplayer::State::Ready ? 1U : 0U);
    SDL_RenderDebugTextFormat(renderer, 20, 32,
                              "%s | %zu PLAYERS | RELAYS %d/3 | %.1fX | %.1f LOOP/S | %s",
                              Multiplayer::modeName(session_->mode()), playerCount,
                              relayCount, currentScale_, measuredLoopHz_,
                              paused_ ? "PAUSED" : "LIVE");
    SDL_SetRenderDrawColor(renderer, 255, 203, 127, 255);
    const std::string status = session_->status();
    SDL_RenderDebugText(renderer, 20, 48, status.substr(0, 111).c_str());

    rect(renderer, 0, 507, 960, 33, {8, 18, 35});
    SDL_SetRenderDrawColor(renderer, 175, 196, 220, 255);
    SDL_RenderDebugText(renderer, 18, 516,
                        "A/D MOVE   SPACE JUMP   HOLD E ON RELAY   P PAUSE   1/2/3 SPEED   F1 SCALE   F2 INFO");

    if (!hasWorld_ || session_->state() != Multiplayer::State::Ready) {
        rect(renderer, 245, 208, 470, 75, {11, 31, 54});
        SDL_SetRenderDrawColor(renderer, 233, 239, 245, 255);
        SDL_RenderDebugText(renderer, 321, 231,
            session_->state() == Multiplayer::State::Failed ? "CONNECTION ERROR" :
            !hasWorld_ ? "WAITING FOR STATION WORLD" : "CONNECTING TO SERVER");
        SDL_RenderDebugText(renderer, 310, 252, "WINDOW AND NETWORK STAY RESPONSIVE");
    }

    if (diagnostics_) {
        rect(renderer, 17, 79, 344, 90, {9, 26, 47});
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
    }
}
