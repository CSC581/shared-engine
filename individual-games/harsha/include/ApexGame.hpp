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

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class ApexGame : public Game {
public:
    ApexGame(Engine& engine, std::unique_ptr<Multiplayer::Session> session = {});
    // Handlers capture this, and engine.events() outlives the game.
    ~ApexGame() override;

    // Input arrives as InputEvents; see onInput() and applyHeldKeys().
    void handleInput(Engine&) override {}
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
    // Air dash: sideways only, so it adds no height (design 12.5).
    static constexpr float dashSpeed = 900.0F;
    static constexpr float dashSeconds = 0.15F;
    // Share of a slingshot rail's vertical velocity added to a jump off it.
    static constexpr float slingshotVerticalShare = 0.5F;
    // Slop for the direction-aware landing check (float drift on the surface).
    static constexpr float landingTolerance = 4.0F;
    // Feet this close to a top after the collision pass were pushed onto it.
    // Far tighter than landingTolerance, or a falling climber would hover.
    static constexpr float pushLandingTolerance = 0.01F;

    static constexpr float playerWidth = 50.0F;
    static constexpr float playerHeight = 70.0F;

    static constexpr int roomCount = 6;

    // On game time, so a pause holds it. Lets the death read on screen.
    static constexpr std::int64_t respawnDelayTics = kGameTicsPerSecond * 2 / 5;

    // Dark brown a crumbling moss ledge fades toward; also half its debris.
    static constexpr Color crumbledColor{110, 80, 45};

    void onInput(const Event& event);
    bool isHeld(SDL_Scancode key) const { return keysHeld_[key]; }
    void applyHeldKeys(float deltaTime);
    void tryDash(float direction);
    void updateDash(float deltaTime);
    void onCollision(const Event& event);
    void handleCollisions();
    void applySlingshotFling();
    void onDeath(const Event& event);
    void onSpawn(const Event& event);
    // Raises the SpawnEvent that stands the climber on a spawn marker.
    void raiseSpawnAt(const Transform& marker, std::int64_t delayTics = 0);
    // Puts the climber at rest at this position, camera snapped.
    void placeAt(float x, float y);
    const Transform* findSpawnMarker(int room, int slot);
    void updateCamera(float deltaTime);
    float cameraTarget() const;
    int roomAt(float worldY) const;
    void snapCamera();
    void updatePlayerPhysics(float deltaTime);
    void updateNetwork();
    void updateCrumbles(float deltaTime);
    void spawnDebris(const Rect& ledge);

    Engine& engine_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
    // Kept from InputEvents, indexed by scancode.
    std::array<bool, SDL_SCANCODE_COUNT> keysHeld_{};

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
    // Wind zone the climber is inside, set and cleared by onCollision() (main
    // thread, between frames' worker runs); read by the player worker.
    const WindZone* wind_ = nullptr;
    // For drawing the wind cue: the (only) wind zone, its push this frame,
    // and how far the streaks have drifted.
    const GameObject* windZoneObject_ = nullptr;
    float windNow_ = 0.0F;
    float windStreakOffset_ = 0.0F;

    // Ledges with a Crumble (the moss). Level objects are never destroyed,
    // so the pointers stay valid; ticked on the main thread.
    std::vector<GameObject*> crumbles_;
    // Bits of a broken ledge: drawn shapes only, not World objects.
    struct Debris {
        float x;
        float y;
        float velocityX;
        float velocityY;
        float life; // seconds left
        float size;
    };
    std::vector<Debris> debris_;

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
    // Between a DeathEvent and its delayed SpawnEvent: switched off and hidden.
    bool isDead_ = false;
    int deaths_ = 0;

    // Vertical scroll: world Y minus camera_ is screen Y. It eases toward the
    // top of cameraRoom_, the room whose hidden RoomZone holds the climber.
    float camera_ = 0.0F;
    int cameraRoom_ = 0;

    float chargePower_ = 0.0F;
    float aimDirection_ = 0.0F;
    // Foot Y before this frame's move; used for landing detection.
    float previousPlayerBottom_ = 0.0F;

    bool isCharging_ = false;
    bool isOnGround_ = false;

    // One dash per airtime; time left in the current one.
    bool dashUsed_ = false;
    float dashDirection_ = 0.0F;
    float dashTimeLeft_ = 0.0F;

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
