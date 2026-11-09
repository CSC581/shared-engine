#pragma once

#include "EventManager.hpp"
#include "FrameTime.hpp"
#include "FrameWorkers.hpp"
#include "Game.hpp"
#include "Multiplayer.hpp"
#include "NetworkEventBridge.hpp"
#include "NetworkWorldSync.hpp"
#include "ReplaySystem.hpp"
#include "StarfallSalvageEvents.hpp"
#include "StarfallSalvageLevel.hpp"
#include "StarfallSalvageRules.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

class Engine;

class StarfallSalvageGame final : public Game {
public:
    // `stationAlarms` false stops this player from setting off alarms when it
    // is the alarm director.
    StarfallSalvageGame(Engine& engine, Multiplayer::Config config, bool stationAlarms = true);
    ~StarfallSalvageGame() override;

    StarfallSalvageGame(const StarfallSalvageGame&) = delete;
    StarfallSalvageGame& operator=(const StarfallSalvageGame&) = delete;

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void update(const FrameTime& time, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    struct InputFrame {
        float axis = 0.0F;
        bool jump = false;
        bool hold = false;
    };

    // What the signal worker reads: a copy taken before the workers start.
    struct FrameJob {
        Multiplayer::PlayerId localId = 0;
        std::vector<Multiplayer::Player> remote;
    };

    // An astronaut or drone as one replay frame remembers it. Both are drawn
    // as sprites, so the replay keeps them in its frame state string.
    struct ReplaySprite {
        bool drone = false;
        std::uint32_t id = 0;
        float x = 0.0F;
        float y = 0.0F;
        bool flipped = false;
        bool down = false;
    };

    void onInput(const Event& event);
    bool held(int scancode) const;
    void toggleManualRecording();
    void toggleReplay();
    void updateReplay();
    std::string recordFrameState() const;
    std::vector<ReplaySprite> replaySprites() const;
    std::vector<StarfallSalvage::Claim> teammateClaims() const;
    bool teammateCanHelp() const;
    // The connected player with the lowest id, who sets off station alarms
    // for everyone; 0 before this player has an id.
    std::uint32_t alarmDirector() const;
    void directAlarms(float dt);
    bool collectCargo(std::size_t crate);
    bool findPlayer(std::uint32_t id, Rect& bounds);

    void updateSignals(const FrameJob& job, float deltaTime);
    // This frame's entry for a remote player, or null.
    const Multiplayer::Player* remotePlayer(std::uint32_t id) const;
    std::string remoteName(std::uint32_t id) const;
    // Upper case; 0 or this player's own id means this player.
    std::string nameOf(std::uint32_t id) const;

    void renderStation(SDL_Renderer* renderer, float cameraX, const World& world, bool live) const;
    void renderReplay(SDL_Renderer* renderer) const;
    void renderCrewOverlays(SDL_Renderer* renderer) const;
    void renderBlackout(SDL_Renderer* renderer) const;
    void renderHud(SDL_Renderer* renderer) const;

    Engine& engine_;

    // Network updates keep flowing while the local simulation is paused.
    Timeline networkTime_;
    std::unique_ptr<Multiplayer::Session> session_;
    Multiplayer::Config config_;

    // The scene. world_ comes before everything that points into it.
    World world_;
    StarfallSalvage::Station station_;
    GameObject* player_ = nullptr;
    StarfallSalvage::Astronaut* astronaut_ = nullptr;
    NetworkWorldSync sync_;

    // Gameplay on events, network events and replay. Built after the scene
    // they point into; destroyed before it.
    std::unique_ptr<StarfallSalvage::StationEvents> stationEvents_;
    std::unique_ptr<NetworkEventBridge> bridge_;
    ReplaySystem replay_;
    std::vector<EventManager::SubscriptionId> subscriptions_;

    // Keys held, from InputEvent; read at the start of update().
    std::unordered_set<int> heldKeys_;
    bool jumpQueued_ = false;
    bool reviveChord_ = false;
    bool reviveArrowChord_ = false;

    // Replay: the last 10 s are always recorded unless a manual clip (F9) is
    // being recorded or waiting to be played.
    bool manualRecording_ = false;
    bool debriefShown_ = false;
    bool debriefActive_ = false;
    bool wasPlaying_ = false;
    double playbackSpeed_ = 1.0;

    // Station alarms, set off by whichever player is the director.
    bool stationAlarms_ = true;
    float nextAlarmSeconds_ = 0.0F;
    std::mt19937 random_{std::random_device{}()};

    InputFrame input_{};
    std::vector<Multiplayer::Player> remote_;
    std::vector<Multiplayer::Platform> platforms_;
    StarfallSalvage::MissionStatus remoteMission_{};
    StarfallSalvage::RelayStatus relays_{};
    std::uint8_t cargoMask_ = 0;
    std::uint8_t personalCargoMask_ = 0;
    std::uint8_t creditedCargoMask_ = 0;
    StarfallSalvage::CargoWinner cargoWinner_{};
    std::string winnerName_;
    float cargoFlashSeconds_ = 0.0F;
    // Latched: once the relays have come online together, the mission stays
    // complete for this player.
    bool missionComplete_ = false;
    // The pad or plate held on the last live frame, kept during a replay so
    // that watching one does not let go of a relay.
    int lastControl_ = -1;
    bool gateOpen_ = false;
    bool hasWorld_ = false;
    bool spawned_ = false;
    float pulse_ = 0.0F;
    float cameraX_ = 0.0F;
    bool cameraInitialized_ = false;
    bool diagnostics_ = false;

    // Read by the workers; written only before runFrame().
    FrameJob frameJob_{};
    FrameTime frameTime_{0.0, 0};
    bool runPhysics_ = false;
    std::uint64_t physicsFrames_ = 0;
    std::uint64_t signalFrames_ = 0;

    // When the next frame is due, on a fixed schedule; 0 before the first frame.
    std::int64_t nextLoopStartNs_ = 0;
    std::int64_t rateSampleStartNs_ = 0;
    std::uint64_t rateLoopCount_ = 0;
    std::uint64_t ratePublishCount_ = 0;
    double measuredLoopHz_ = 0.0;
    double publishCallsHz_ = 0.0;
    double currentScale_ = 1.0;
    double gameSeconds_ = 0.0;
    std::int64_t loopTics_ = 0;
    bool paused_ = false;
    bool proportionalScale_ = true;
    std::string lastStatus_;
    std::size_t lastRemoteCount_ = 0;

    // Destroy first so workers join before the frame inputs or game state die.
    FrameWorkers workers_;
};
