// Apex Ascent gameplay: climb a tower of platforms one charged jump at a time.
// Simulation lives in ApexGame.cpp, drawing and HUD in ApexGameRender.cpp.

#pragma once

#include "ApexComponents.hpp"
#include "Engine.hpp"
#include "FrameTime.hpp"
#include "FrameWorkers.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "Multiplayer.hpp"
#include "NetworkWorldSync.hpp"
#include "World.hpp"
#include "animation/PlayerAnimation.hpp"

#include <SDL3/SDL.h>

#include <memory>

class ApexGame : public Game {
public:
    ApexGame(const Engine& engine, std::unique_ptr<Multiplayer::Session> session = {});

    void handleInput(Engine& engine) override;
    // The engine calls the FrameTime overload; World needs the FrameTime.
    // The float one is pure virtual in Game, so it stays as a forwarder.
    void update(const FrameTime& time, Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 2200.0F;

    static constexpr float baseJumpSpeed = 480.0F;
    static constexpr float maxChargePower = 1500.0F;
    static constexpr float chargeSpeed = 1100.0F;
    static constexpr float horizontalJumpRatio = 0.55F;

    static constexpr float walkSpeed = 300.0F;
    // Share of a slingshot rail's vertical velocity added to a jump off it.
    static constexpr float slingshotVerticalShare = 0.5F;
    // Slop for the direction-aware landing check (float drift on the surface).
    static constexpr float landingTolerance = 4.0F;

    static constexpr float playerWidth = 50.0F;
    static constexpr float playerHeight = 70.0F;

    static constexpr int roomCount = 6;

    void handleCollisions();
    void onPlayerCollide(GameObject& other);
    void applySlingshotFling();
    // Puts the climber standing at a spawn marker, at rest, camera snapped.
    void placeAt(const Transform& marker);
    const Transform* findSpawnMarker(int room, int slot);
    void onPlayerEnter(GameObject& other);
    void onPlayerExit(GameObject& other);
    void updateCamera(float deltaTime);
    float cameraTarget() const;
    void snapCamera();
    void updatePlayerPhysics(float deltaTime);
    void updateNetwork();

    // Every object in the scene: platforms, the local player and, online,
    // the remote players and server-owned platforms NetworkWorldSync mirrors.
    World world_;
    // Owned by world_ and never destroyed, so the pointer stays valid.
    GameObject* player_ = nullptr;
    // The player's components, cached once like a component's onAttach does.
    Transform* playerTransform_ = nullptr;
    Motion* playerMotion_ = nullptr;
    Gravity* playerGravity_ = nullptr;
    // Online only. Declared after world_, which it refers to, so it is
    // destroyed first.
    std::unique_ptr<NetworkWorldSync> sync_;
    // Platform stood on last frame, for carrying; 0 = none (World ids start
    // at 1). An id, not a pointer: online platforms can be destroyed.
    ObjectId groundedPlatformId_ = 0;
    // This frame's time, stashed for the worker threads.
    FrameTime frameTime_{};
    // Walk direction from this frame's keys (-1, 0, +1); only set on ground.
    float walkIntent_ = 0.0F;
    // Surface::friction of the platform underfoot, refreshed each frame.
    float groundFriction_ = Surface::ground;
    // Wind zone the climber is inside, set and cleared by the player's
    // trigger callbacks (main thread); read by the player worker.
    const WindZone* wind_ = nullptr;
    // For drawing the wind cue: the (only) wind zone, its push this frame,
    // and how far the streaks have drifted.
    const GameObject* windZoneObject_ = nullptr;
    float windNow_ = 0.0F;
    float windStreakOffset_ = 0.0F;

    // Milestone 4: climb-loop clips + facing driven by gameplay.
    PlayerAnimation playerAnim_;

    // Optional online presence. The game uses the same surface for either
    // client-server or peer-to-peer deployment.
    std::unique_ptr<Multiplayer::Session> session_;

    float viewWidth_;
    float viewHeight_;
    float worldWidth_;
    float worldHeight_;

    // Where a death sends the climber: the marker of the highest room whose
    // checkpoint ledge it has stood on (room 0: its starting slot). Markers
    // are never destroyed, so the pointer stays valid.
    const Transform* checkpoint_ = nullptr;
    int checkpointRoom_ = 0;
    // Online: moved to slot (id - 1) % 4 once, when the server assigns an id.
    bool onlineSlotPlaced_ = false;
    // Set by a death zone's onEnter, acted on once the collision pass ends.
    bool pendingRespawn_ = false;
    int deaths_ = 0;

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

    // Section 3: platforms and player physics each run on their own thread
    // every frame. Declared last so its threads are joined before any member
    // they use is destroyed.
    FrameWorkers workers_;
};
