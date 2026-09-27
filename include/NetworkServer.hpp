#pragma once

#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Network {

struct SpawnPoint {
    float x = 0.0F;
    float y = 0.0F;
};

// Straight-line ping-pong path. Positions are in the same space clients use for
// players (arena-local for the network demo). Speed is units per second of the
// server's TimeSource (real nanoseconds when using RealTimeClock).
struct PlatformPath {
    std::uint32_t id = 0;
    float startX = 0.0F;
    float startY = 0.0F;
    float endX = 0.0F;
    float endY = 0.0F;
    float speed = 80.0F;
    float width = 96.0F;
    float height = 20.0F;
};

// The application supplies initial positions and optional moving platforms.
// Character simulation belongs to clients; the server manages membership,
// stores reported positions, and authors platform poses.
struct ServerConfig {
    std::vector<SpawnPoint> spawnPoints;
    std::vector<PlatformPath> platforms;
    std::size_t maxPlayers = 8;
    // In the supplied clock's units; the default assumes real nanoseconds.
    std::int64_t inactivityTimeoutTics = 3 * kNsPerSec;
};

// Thread-safe session/state store, independent of SDL and ZeroMQ.
// Positions are trusted client reports, not validated game physics.
//
// Every public method may be called from any thread (Section 4 per-client
// workers share one instance). std::mutex is not recursive, so public methods
// must not call each other while holding the lock — private helpers assume the
// lock is already held.
class NetworkServer {
public:
    NetworkServer(const TimeSource& clock, ServerConfig config);

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    // Advances server-owned platforms on real time and expires idle clients.
    void update();

    // Processes one decoded transport message and returns its reply message.
    Message handle(const Message& message);

    WorldSnapshot snapshot() const;
    std::size_t playerCount() const;

private:
    struct ActivePlayer {
        PlayerState state;
        SessionToken sessionToken;
        std::uint64_t lastSequence = 0;
        std::int64_t lastHeard = 0;
    };

    struct ActivePlatform {
        PlatformPath path;
        PlatformState state;
        // Distance along the A↔B segment; ping-pong via velocity sign.
        float distance = 0.0F;
        float velocity = 0.0F;
        float pathLength = 0.0F;
    };

    // Callers must hold mutex_.
    void expireInactivePlayers(std::int64_t now);
    void advancePlatforms(std::int64_t now);
    WorldSnapshot buildSnapshot() const;
    WorldStateSnapshot buildWorldState() const;
    PlayerState spawnPlayer(PlayerId id) const;

    const TimeSource& clock_;
    ServerConfig config_;
    mutable std::mutex mutex_;
    PlayerId nextPlayerId_ = 1;
    std::uint64_t serverTick_ = 0;
    std::uint64_t worldRevision_ = 0;
    std::int64_t lastPlatformUpdate_ = 0;
    bool platformClockStarted_ = false;
    std::unordered_map<PlayerId, ActivePlayer> players_;
    std::unordered_map<SessionToken, PlayerId> playerIdsByToken_;
    std::vector<ActivePlatform> platforms_;
};

} // namespace Network
