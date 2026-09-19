// Apex Ascent: climb a tower of platforms one charged jump at a time.

#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "ApexNetworkConfig.hpp"
#include "NetworkClient.hpp"
#include "Physics.hpp"
#include "animation/PlayerAnimation.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* connectionLabel(Network::ConnectionState state)
{
    switch (state) {
    case Network::ConnectionState::Connecting:
        return "Connecting";
    case Network::ConnectionState::Connected:
        return "Connected";
    case Network::ConnectionState::Error:
        return "Error";
    case Network::ConnectionState::Disconnected:
        return "Disconnected";
    }
    return "Unknown";
}

// Distinct tints for remote climbers (ghosts); local player keeps the sprite.
void ghostColor(Network::PlayerId playerId, Uint8& red, Uint8& green, Uint8& blue)
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

class ApexAscent : public Game {
public:
    // joinEndpoint empty = offline. Non-empty starts NetworkClient on real time.
    ApexAscent(const Engine& engine, std::string joinEndpoint = {});
    ~ApexAscent() override;

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

    // Optional online presence (APX-M1). Platforms stay local until APX-M2.
    std::unique_ptr<Network::NetworkClient> client_;

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

ApexAscent::ApexAscent(const Engine& engine, std::string joinEndpoint)
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

    if (!playerAnim_.load(engine.getRenderer())) {
        std::cerr << "Apex Ascent: player sprite load failed; falling back to rect draw "
                     "(expected media/apex-ascent/Idle.png next to the binary).\n";
    }

    if (!joinEndpoint.empty()) {
        // Keep room-0 spawn; ignore server arena spawn points (demo coords).
        client_ = std::make_unique<Network::NetworkClient>(engine.realTime(),
                                                           std::move(joinEndpoint));
        client_->start();
        std::cout << "Apex Ascent (online): peers are ghosts; moving platform is server-owned.\n"
                     "Start server with: ./apex-network-server\n"
                     "A/D aim, Space charge. P pause, 1/2/3 = 0.5x/1x/2x game time. Esc quit.\n";
    } else {
        std::cout << "Apex Ascent: A/D to aim, hold Space to charge a jump, "
                     "release to leap. F1 to toggle scaling, Esc to quit.\n"
                     "Optional: ./apex-ascent --join [tcp://host:port]\n";
    }
}

ApexAscent::~ApexAscent()
{
    if (client_) {
        client_->leave();
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
    if (client_) {
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
    if (!client_) {
        return;
    }

    // poll() is called at the start of update() so platform poses are fresh.
    if (client_->state() == Network::ConnectionState::Connected) {
        // Local climb is authoritative; never rewind from snapshot (server spawn
        // is only a join hint — we keep room-0 placement from the client).
        client_->submitPosition(player_.getX(), player_.getY());
    }
}

void ApexAscent::update(float deltaTime, Engine& engine)
{
    currentScaleMode_ = engine.getScaleMode();

    // Refresh snapshots before moving-platform carry (server poses).
    // NetworkClient uses realTime, so pause/scale never freezes the link.
    if (client_) {
        client_->poll();

        const Timeline& gameTime = engine.gameTime();
        gameTimePaused_ = gameTime.isPaused();
        gameTimeScale_ = gameTime.scale();

        char title[128];
        if (gameTimePaused_) {
            std::snprintf(title, sizeof(title), "Apex Ascent (online) | PAUSED | #%u",
                          static_cast<unsigned>(client_->playerId()));
        } else {
            std::snprintf(title, sizeof(title), "Apex Ascent (online) | %.1fx | #%u",
                          gameTimeScale_, static_cast<unsigned>(client_->playerId()));
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
    if (client_ && client_->state() == Network::ConnectionState::Connected) {
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
    const Network::WorldSnapshot& snapshot = client_->snapshot();

    for (MovingPlatformPath& path : movingPlatformPaths_) {
        if (path.networkId == 0) {
            continue;
        }

        const Network::PlatformState* match = nullptr;
        for (const Network::PlatformState& platform : snapshot.platforms) {
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
    if (client_ && client_->state() == Network::ConnectionState::Connected) {
        for (const Network::PlayerState& remote : client_->snapshot().players) {
            if (remote.id == client_->playerId()) {
                continue;
            }
            Uint8 red = 0;
            Uint8 green = 0;
            Uint8 blue = 0;
            ghostColor(remote.id, red, green, blue);

            const float screenTop = remote.y - camera_;
            const float screenBottom = screenTop + playerHeight;
            if (screenBottom > 0.0F && screenTop < viewHeight_) {
                if (playerAnim_.isLoaded()) {
                    // Pose-only protocol: idle tinted sprite until a future shared
                    // presentation channel exists (keep anim out of network-core).
                    playerAnim_.drawGhost(renderer, remote.x, remote.y, camera_, playerWidth,
                                          playerHeight, PlayerAnimation::Clip::Idle, 0, true, red,
                                          green, blue);
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

    if (client_) {
        char netHud[160];
        if (!client_->error().empty()) {
            std::snprintf(netHud, sizeof(netHud), "Net: %s — %s", connectionLabel(client_->state()),
                          client_->error().c_str());
            SDL_SetRenderDrawColor(renderer, 255, 140, 120, 255);
        } else {
            std::snprintf(netHud, sizeof(netHud),
                          "Net: %s | you #%u | %zu climbers | server platforms",
                          connectionLabel(client_->state()),
                          static_cast<unsigned>(client_->playerId()),
                          client_->snapshot().players.size());
            SDL_SetRenderDrawColor(renderer, 180, 220, 255, 255);
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

std::string parseJoinEndpoint(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--join") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                return argv[i + 1];
            }
            return "tcp://127.0.0.1:5555";
        }
        // Bare endpoint for convenience: ./apex-ascent tcp://127.0.0.1:5555
        if (arg.rfind("tcp://", 0) == 0) {
            return arg;
        }
    }
    return {};
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const std::string joinEndpoint = parseJoinEndpoint(argc, argv);

        // 900x900 design resolution (fits ordinary screens in Constant mode).
        Engine engine(joinEndpoint.empty() ? "Apex Ascent" : "Apex Ascent (online)", 900, 900);
        engine.setClearColor(18, 20, 32);

        ApexAscent game(engine, joinEndpoint);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
