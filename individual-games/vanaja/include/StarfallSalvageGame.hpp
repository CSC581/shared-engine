#pragma once

#include "FrameTime.hpp"
#include "FrameWorkers.hpp"
#include "Game.hpp"
#include "Multiplayer.hpp"
#include "NetworkWorldSync.hpp"
#include "StarfallSalvageLevel.hpp"
#include "StarfallSalvageRules.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Engine;

class StarfallSalvageGame final : public Game {
public:
    StarfallSalvageGame(Engine& engine, Multiplayer::Config config);
    ~StarfallSalvageGame() override = default;

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

    void updateSignals(const FrameJob& job, float deltaTime);
    std::string remoteName(std::uint32_t id) const;

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
    bool missionComplete_ = false;
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
