#pragma once

#include "Entity.hpp"
#include "Game.hpp"
#include "Multiplayer.hpp"
#include "SignalBloomRules.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Engine;

class SignalBloomGame final : public Game {
public:
    SignalBloomGame(Engine& engine, Multiplayer::Config config);
    ~SignalBloomGame() override;

    SignalBloomGame(const SignalBloomGame&) = delete;
    SignalBloomGame& operator=(const SignalBloomGame&) = delete;

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

    struct FrameJob {
        FrameTime time{};
        InputFrame input{};
        bool canMove = false;
        Multiplayer::PlayerId localId = 0;
        std::vector<Multiplayer::Platform> platforms;
        std::vector<Multiplayer::Player> remote;
    };

    void physicsLoop();
    void signalLoop();
    void workerDone();
    void simulatePlayer(const FrameJob& job);
    void resolveBlocker(const Rect& blocker, std::uint32_t platformId, const Rect& previous);
    void respawn();

    std::unique_ptr<Multiplayer::Session> session_;
    Multiplayer::Config config_;
    Entity player_{76.0F, 380.0F, 30.0F, 40.0F};
    InputFrame input_{};
    std::vector<Multiplayer::Player> remote_;
    std::vector<Multiplayer::Platform> platforms_;
    SignalBloom::RelayStatus relays_{};
    bool hasWorld_ = false;
    bool spawned_ = false;
    bool grounded_ = false;
    std::uint32_t supportId_ = 0;
    std::vector<Multiplayer::Platform> lastPhysicsPlatforms_;
    float pulse_ = 0.0F;
    bool diagnostics_ = false;

    std::mutex frameMutex_;
    std::condition_variable frameCv_;
    FrameJob frameJob_{};
    std::uint64_t generation_ = 0;
    unsigned workersDone_ = 0;
    bool stopping_ = false;
    std::thread physicsThread_;
    std::thread signalThread_;
    std::array<bool, 3> remoteOccupied_{{false, false, false}};
    std::uint64_t physicsFrames_ = 0;
    std::uint64_t signalFrames_ = 0;

    std::int64_t lastLoopStartNs_ = 0;
    std::int64_t rateSampleStartNs_ = 0;
    std::uint64_t rateLoopCount_ = 0;
    std::uint64_t ratePublishCount_ = 0;
    double measuredLoopHz_ = 0.0;
    double publishCallsHz_ = 0.0;
    double currentScale_ = 1.0;
    double gameSeconds_ = 0.0;
    std::int64_t loopTics_ = 0;
    bool paused_ = false;
    std::string lastStatus_;
    std::size_t lastRemoteCount_ = 0;
};
