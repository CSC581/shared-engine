#pragma once

#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Network {

struct SpawnPoint {
    float x = 0.0F;
    float y = 0.0F;
};

// A game/server executable chooses its own level bounds, speed, and spawn
// points, then passes them to the generic authoritative simulation below.
struct ServerConfig {
    float arenaWidth = 0.0F;
    float arenaHeight = 0.0F;
    float playerSize = 0.0F;
    float playerSpeed = 0.0F;
    std::vector<SpawnPoint> spawnPoints;
    std::size_t maxPlayers = 8;
    std::int64_t ticsPerSecond = kNsPerSec;
    std::int64_t inactivityTimeoutTics = 3 * kNsPerSec;
};

// The authoritative world. It has no SDL or ZeroMQ dependency, which lets
// tests drive it directly with ManualClock and lets the transport stay thin.
class NetworkServer {
public:
    NetworkServer(const TimeSource& clock, ServerConfig config);

    // Advances active player positions and removes timed-out clients.
    void update();

    // Processes one decoded transport message and returns its reply message.
    Message handle(const Message& message);

    WorldSnapshot snapshot() const;
    std::size_t playerCount() const;

private:
    struct ActivePlayer {
        PlayerState state;
        MovementInput input;
        std::int64_t lastHeard = 0;
    };

    void expireInactivePlayers(std::int64_t now);
    WorldSnapshot snapshotLocked() const;
    PlayerState spawnPlayer(PlayerId id) const;

    const TimeSource& clock_;
    ServerConfig config_;
    std::int64_t lastUpdate_;
    PlayerId nextPlayerId_ = 1;
    std::uint64_t serverTick_ = 0;
    std::unordered_map<PlayerId, ActivePlayer> players_;
};

} // namespace Network
