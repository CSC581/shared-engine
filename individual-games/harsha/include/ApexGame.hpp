// Apex Ascent gameplay: climb a tower of platforms one charged jump at a time.
// Simulation lives in ApexGame.cpp, drawing and HUD in ApexGameRender.cpp.

#pragma once

#include "Engine.hpp"
#include "Entity.hpp"
#include "FrameWorkers.hpp"
#include "Game.hpp"
#include "Multiplayer.hpp"
#include "animation/PlayerAnimation.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class ApexGame : public Game {
public:
    ApexGame(const Engine& engine, std::unique_ptr<Multiplayer::Session> session = {});

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
    void updatePlayerPhysics(float deltaTime);
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

    // How far the platform the player stands on moved this frame. The platform
    // worker records it; the main thread applies it after both workers finish,
    // so the two threads never write the player at the same time.
    float carryDx_ = 0.0F;
    float carryDy_ = 0.0F;

    // Section 3: platforms and player physics each run on their own thread
    // every frame. Declared last so its threads are joined before any member
    // they use is destroyed.
    FrameWorkers workers_;
};
