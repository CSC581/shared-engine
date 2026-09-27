// Apex Ascent: climb a tower of platforms one charged jump at a time.

#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "ApexListenServer.hpp"
#include "ApexNetworkConfig.hpp"
#include "Multiplayer.hpp"
#include "Physics.hpp"
#include "animation/PlayerAnimation.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* connectionLabel(Multiplayer::State state)
{
    switch (state) {
    case Multiplayer::State::Connecting:
        return "Connecting";
    case Multiplayer::State::Ready:
        return "Ready";
    case Multiplayer::State::Failed:
        return "Failed";
    }
    return "Unknown";
}

const char* authorityLabel(Multiplayer::AuthorityState state)
{
    switch (state) {
    case Multiplayer::AuthorityState::NotConfigured:
        return "Not configured";
    case Multiplayer::AuthorityState::Connecting:
        return "Connecting";
    case Multiplayer::AuthorityState::Ready:
        return "Ready";
    case Multiplayer::AuthorityState::Failed:
        return "Failed";
    }
    return "Unknown";
}

// Distinct tints for remote climbers (ghosts); local player keeps the sprite.
void ghostColor(Multiplayer::PlayerId playerId, Uint8& red, Uint8& green, Uint8& blue)
{
    static constexpr Uint8 colors[][3] = {
        {80, 220, 255}, {255, 110, 130}, {255, 210, 80}, {150, 240, 140},
        {190, 140, 255}, {255, 160, 90}, {100, 170, 255}, {245, 120, 220},
    };
    const auto& color = colors[(playerId - 1) % (sizeof(colors) / sizeof(colors[0]))];
    red = color[0];
    green = color[1];
    blue = color[2];
}

struct RemoteAnimation {
    PlayerAnimation::Clip clip = PlayerAnimation::Clip::Idle;
    int frame = 0;
    bool facingRight = true;
};

std::string encodeAnimation(const PlayerAnimation& animation)
{
    return std::to_string(static_cast<int>(animation.currentClip())) + ' ' +
           std::to_string(animation.currentFrame()) + ' ' +
           (animation.facingRight() ? "1" : "0");
}

bool decodeAnimation(const std::string& data, RemoteAnimation& animation)
{
    std::istringstream input(data);
    input.imbue(std::locale::classic());

    int clip = 0;
    int frame = 0;
    int facing = 0;
    if (!(input >> clip >> frame >> facing)) {
        return false;
    }
    input >> std::ws;
    if (!input.eof() || clip < 0 || clip >= static_cast<int>(PlayerAnimation::Clip::Count) ||
        frame < 0 || frame > 100 || (facing != 0 && facing != 1)) {
        return false;
    }

    animation.clip = static_cast<PlayerAnimation::Clip>(clip);
    animation.frame = frame;
    animation.facingRight = facing == 1;
    return true;
}

class ApexAscent : public Game {
public:
    ApexAscent(const Engine& engine, std::unique_ptr<Multiplayer::Session> session = {});

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
    // networkId != 0: online mode applies server snapshot poses (APX-M2).
    struct MovingPlatformPath {
        std::size_t platformIndex;
        float pointAX;
        float pointAY;
        float pointBX;
        float pointBY;
        float speed;
        bool movingToB = true;
        std::uint32_t networkId = 0;
        bool serverPoseInitialized = false;
    };

    void buildLevel();
    void handleCollisions();
    void updateCamera(float deltaTime);
    void updateMovingPlatforms(float deltaTime);
    void applyServerMovingPlatforms();
    void updateNetwork();
    bool isMovingPlatform(const Entity& platform) const;

    Entity player_;
    std::vector<Entity> platforms_;
    std::vector<MovingPlatformPath> movingPlatformPaths_;
    // Last platform stood on; used to carry the player next frame.
    Entity* groundedMovingPlatform_ = nullptr;

    // Milestone 4: climb-loop clips + facing driven by gameplay.
    PlayerAnimation playerAnim_;

    // Optional online presence. The game uses the same surface for either
    // client-server or peer-to-peer deployment.
    std::unique_ptr<Multiplayer::Session> session_;

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

    // Online timeline proof (APX-M4); unused offline.
    bool gameTimePaused_ = false;
    double gameTimeScale_ = 1.0;
};

ApexAscent::ApexAscent(const Engine& engine, std::unique_ptr<Multiplayer::Session> session)
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

void ApexAscent::handleInput(Engine& engine)
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

void ApexAscent::updateNetwork()
{
    if (!session_) {
        return;
    }

    // update() is called at the start of the frame so platform poses are fresh.
    if (session_->state() == Multiplayer::State::Ready) {
        // Local climb is authoritative; never rewind from snapshot (server spawn
        // is only a join hint — we keep room-0 placement from the client).
        session_->publishLocalPlayer(player_.getX(), player_.getY(), encodeAnimation(playerAnim_));
    }
}

void ApexAscent::update(float deltaTime, Engine& engine)
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

void ApexAscent::applyServerMovingPlatforms()
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

    // Remote climbers: tinted sprites when on-screen; HUD cue when above/below.
    if (session_ && session_->state() == Multiplayer::State::Ready) {
        for (const Multiplayer::Player& remote : session_->remotePlayers()) {
            Uint8 red = 0;
            Uint8 green = 0;
            Uint8 blue = 0;
            ghostColor(remote.id, red, green, blue);

            const float screenTop = remote.y - camera_;
            const float screenBottom = screenTop + playerHeight;
            if (screenBottom > 0.0F && screenTop < viewHeight_) {
                if (playerAnim_.isLoaded()) {
                    RemoteAnimation animation;
                    decodeAnimation(remote.data, animation);
                    playerAnim_.drawGhost(renderer, remote.x, remote.y, camera_, playerWidth,
                                          playerHeight, animation.clip, animation.frame,
                                          animation.facingRight, red, green, blue);
                } else {
                    drawRect({remote.x, remote.y, playerWidth, playerHeight}, red, green, blue);
                }
            } else {
                char cue[48];
                const float cueX = std::fmax(20.0F, std::fmin(remote.x, viewWidth_ - 120.0F));
                if (screenBottom <= 0.0F) {
                    std::snprintf(cue, sizeof(cue), "^ #%u above",
                                  static_cast<unsigned>(remote.id));
                    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
                    SDL_RenderDebugText(renderer, cueX, 8.0F, cue);
                } else {
                    std::snprintf(cue, sizeof(cue), "v #%u below",
                                  static_cast<unsigned>(remote.id));
                    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
                    SDL_RenderDebugText(renderer, cueX, viewHeight_ - 24.0F, cue);
                }
            }
        }
    }

    if (playerAnim_.isLoaded()) {
        playerAnim_.draw(renderer, player_.getX(), player_.getY(), camera_, playerWidth,
                         playerHeight);
    } else {
        drawRect(player_.getBounds(), 240, 240, 255);
    }

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

    if (session_) {
        char netHud[256];
        if (session_->state() != Multiplayer::State::Ready) {
            std::snprintf(netHud, sizeof(netHud), "Net: %s — %s",
                          connectionLabel(session_->state()), session_->status().c_str());
            SDL_SetRenderDrawColor(renderer, 255, 140, 120, 255);
        } else if (session_->authorityState() == Multiplayer::AuthorityState::Failed) {
            std::snprintf(netHud, sizeof(netHud), "Net: Ready | authority Failed — %s",
                          session_->status().c_str());
            SDL_SetRenderDrawColor(renderer, 255, 140, 120, 255);
        } else {
            std::snprintf(netHud, sizeof(netHud),
                          "Net: %s | you #%u | %zu climbers | authority %s",
                          connectionLabel(session_->state()),
                          static_cast<unsigned>(session_->localPlayerId()),
                          session_->remotePlayers().size() + 1,
                          authorityLabel(session_->authorityState()));
            if (session_->authorityState() == Multiplayer::AuthorityState::Connecting) {
                SDL_SetRenderDrawColor(renderer, 255, 205, 100, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 180, 220, 255, 255);
            }
        }
        SDL_RenderDebugText(renderer, 20.0F, 60.0F, netHud);

        char timeHud[96];
        if (gameTimePaused_) {
            std::snprintf(timeHud, sizeof(timeHud), "Game time: PAUSED  (P  1=0.5x  2=1x  3=2x)");
        } else {
            std::snprintf(timeHud, sizeof(timeHud), "Game time: %.1fx  (P  1=0.5x  2=1x  3=2x)",
                          gameTimeScale_);
        }
        SDL_SetRenderDrawColor(renderer, 200, 230, 180, 255);
        SDL_RenderDebugText(renderer, 20.0F, 80.0F, timeHud);
    }
}

} // namespace

namespace {

struct ApexOptions {
    Multiplayer::Config network;
    bool online = false;
    bool hostAuthority = false;
    bool help = false;
};

void printUsage()
{
    std::cout <<
        R"(Apex Ascent

  ./build/apex-ascent                              Offline
  ./build/apex-ascent --join [SERVER]              Legacy client-server shortcut
  ./build/apex-ascent --mode client-server         Client-server online mode
  ./build/apex-ascent --mode peer-to-peer [options]

Network options:
  --mode MODE       client-server or peer-to-peer
  --name TEXT       Display name
  --server ENDPOINT Dedicated/listen authority for shared platforms

Peer-to-peer options:
  --id N            Unique peer id, 1..4294967295
  --port P          Introduction port; P+1 broadcasts state (default 7200)
  --peer ENDPOINT   Introduction endpoint of any running peer
  --advertise HOST  Reachable host this peer advertises
  --host            Host the Apex platform authority in this process
)";
}

bool parseOptions(int argc, char* argv[], ApexOptions& options)
{
    options.network.basePort = 7200;

    auto value = [&](int& index, const char* flag, std::string& out) {
        if (index + 1 >= argc) {
            std::cerr << "apex-ascent: " << flag << " requires a value\n";
            return false;
        }
        out = argv[++index];
        return true;
    };

    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        std::string text;

        if (arg == "--help" || arg == "-h") {
            printUsage();
            options.help = true;
            return false;
        }
        if (arg == "--join") {
            options.online = true;
            options.network.mode = Multiplayer::Mode::ClientServer;
            if (index + 1 < argc && argv[index + 1][0] != '-') {
                options.network.serverEndpoint = argv[++index];
            }
            continue;
        }
        if (arg == "--mode") {
            if (!value(index, "--mode", text) || !Multiplayer::parseMode(text, options.network.mode)) {
                std::cerr << "apex-ascent: --mode must be client-server or peer-to-peer\n";
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--name") {
            if (!value(index, "--name", options.network.playerName)) {
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--server") {
            if (!value(index, "--server", options.network.serverEndpoint)) {
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--peer") {
            if (!value(index, "--peer", text)) {
                return false;
            }
            options.network.bootstrapPeers.push_back(text);
            options.online = true;
            continue;
        }
        if (arg == "--advertise") {
            if (!value(index, "--advertise", options.network.advertiseHost)) {
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--id") {
            if (!value(index, "--id", text)) {
                return false;
            }
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || parsed == 0 ||
                parsed > std::numeric_limits<Multiplayer::PlayerId>::max()) {
                std::cerr << "apex-ascent: --id must be a non-zero 32-bit integer\n";
                return false;
            }
            options.network.peerId = static_cast<Multiplayer::PlayerId>(parsed);
            options.online = true;
            continue;
        }
        if (arg == "--port") {
            if (!value(index, "--port", text)) {
                return false;
            }
            char* end = nullptr;
            const long parsed = std::strtol(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || parsed < 1024 || parsed > 65534) {
                std::cerr << "apex-ascent: --port must be between 1024 and 65534\n";
                return false;
            }
            options.network.basePort = static_cast<int>(parsed);
            options.online = true;
            continue;
        }
        if (arg == "--host") {
            options.hostAuthority = true;
            options.online = true;
            continue;
        }
        // Bare endpoint for convenience: ./apex-ascent tcp://127.0.0.1:5555
        if (arg.rfind("tcp://", 0) == 0) {
            options.online = true;
            options.network.mode = Multiplayer::Mode::ClientServer;
            options.network.serverEndpoint = arg;
            continue;
        }

        std::cerr << "apex-ascent: unknown option " << arg << "\n\n";
        printUsage();
        return false;
    }

    if (options.network.playerName.empty()) {
        options.network.playerName = "apex-" + std::to_string(options.network.peerId);
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        ApexOptions options;
        if (!parseOptions(argc, argv, options)) {
            return options.help ? 0 : 1;
        }

        std::unique_ptr<ApexListenServer> listenServer;
        if (options.hostAuthority) {
            if (options.network.mode != Multiplayer::Mode::PeerToPeer) {
                std::cerr << "apex-ascent: --host requires --mode peer-to-peer\n";
                return 1;
            }

            const std::size_t colon = options.network.serverEndpoint.rfind(':');
            if (options.network.serverEndpoint.rfind("tcp://", 0) != 0 ||
                colon == std::string::npos || colon <= 6 ||
                colon + 1 >= options.network.serverEndpoint.size()) {
                std::cerr << "apex-ascent: --host requires a tcp server endpoint with a port\n";
                return 1;
            }
            const std::string bindEndpoint =
                "tcp://*" + options.network.serverEndpoint.substr(colon);
            listenServer = std::make_unique<ApexListenServer>(bindEndpoint);
            listenServer->start();
            std::cout << "Hosting Apex platform authority on " << bindEndpoint << '\n';
        }

        // 900x900 design resolution (fits ordinary screens in Constant mode).
        Engine engine(options.online ? "Apex Ascent (online)" : "Apex Ascent", 900, 900);
        engine.setClearColor(18, 20, 32);

        std::unique_ptr<Multiplayer::Session> session;
        if (options.online) {
            std::cout << "Starting Apex in " << Multiplayer::modeName(options.network.mode)
                      << " mode\n";
            session = Multiplayer::Session::open(std::move(options.network), engine.realTime());
        }

        ApexAscent game(engine, std::move(session));
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Apex Ascent failed to start: " << error.what() << '\n';
        return 1;
    }
}
