#pragma once

#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "World.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Network {

struct SpawnPoint {
    float x = 0.0F;
    float y = 0.0F;
};

// A server-owned moving platform's path, driven by a PathMover component, so a
// networked platform can move in any pattern a local one can:
//   Linear    back and forth from start to end (the default)
//   Circular  around (centerX, centerY) at `radius`, starting at the rightmost
//             point; start/end are ignored
// Positions are in the same space clients use for players (arena-local for the
// network demo). Speed is units per second of the server's TimeSource (real
// nanoseconds when using RealTimeClock); on a circle it is speed along the
// rim. The circle fields come last so existing {id, start…, height}
// initialisers still mean a linear path.
struct PlatformPath {
    enum class Shape { Linear, Circular };

    std::uint32_t id = 0;
    float startX = 0.0F;
    float startY = 0.0F;
    float endX = 0.0F;
    float endY = 0.0F;
    float speed = 80.0F;
    float width = 96.0F;
    float height = 20.0F;
    Shape shape = Shape::Linear;
    float centerX = 0.0F;
    float centerY = 0.0F;
    float radius = 0.0F;
};

// The application supplies initial positions and optional moving platforms.
// Character simulation belongs to clients; the server manages membership,
// stores reported positions, and authors platform poses.
struct ServerConfig {
    std::vector<SpawnPoint> spawnPoints;
    std::vector<PlatformPath> platforms;
    // 0 accepts any number of players. Spawn points are reused in turn once
    // there are more players than points.
    std::size_t maxPlayers = 0;
    // In the supplied clock's units; the default assumes real nanoseconds.
    std::int64_t inactivityTimeoutTics = 3 * kNsPerSec;
};

// How much one player has sent, as counted by the server. Accepted updates
// only: a POSITION refused for a stale sequence or a wrong token is not
// counted, so this measures what the server actually took from each client.
struct PlayerTraffic {
    PlayerId id = 0;
    std::string name;
    std::uint64_t acceptedPositions = 0;
};

// Thread-safe session/state store, independent of SDL and ZeroMQ.
// Positions are trusted client reports, not validated game physics.
//
// The scene is kept in the engine's object model, as on every client: each
// player and each platform is a GameObject in a World, tagged with a
// NetworkIdentity. Platforms move by a PathMover component; a player's
// position is its Transform. Session bookkeeping (tokens, sequence numbers,
// liveness) stays beside the World, because it describes a connection rather
// than anything in the scene.
//
// Every public method may be called from any thread (Section 4 per-client
// workers share one instance). std::mutex is not recursive, so public methods
// must not call each other while holding the lock — private helpers assume the
// lock is already held. The World is touched only under that lock.
class NetworkServer {
public:
    NetworkServer(const TimeSource& clock, ServerConfig config);

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    // Advances the World (server-owned platforms) by the real time elapsed
    // since the last call, and expires idle clients. NetworkServerHost calls
    // this from its tick thread; it is the only thing that moves platforms, so
    // the World advances at the tick rate however many clients are sending.
    void update();

    // Processes one decoded transport message and returns its reply message.
    // Does not advance platforms: replies carry the poses of the last update().
    Message handle(const Message& message);

    WorldSnapshot snapshot() const;
    std::size_t playerCount() const;

    // Whether this token currently owns a player. False once the player has
    // left or been expired for inactivity.
    bool hasSession(const SessionToken& sessionToken) const;

    // Per-player accepted-update counts since each player joined. Monotonic
    // for a given player; a caller wanting a rate samples it twice.
    std::vector<PlayerTraffic> traffic() const;

private:
    // A connected player: its connection bookkeeping, and the GameObject that
    // stands for it in the World. Only this class creates or destroys that
    // object, and it erases the entry in the same step, so the pointer is
    // valid for as long as the entry exists.
    struct ActivePlayer {
        GameObject* object = nullptr;
        std::string name;
        std::string data;
        SessionToken sessionToken;
        std::uint64_t lastSequence = 0;
        std::int64_t lastHeard = 0;
        std::uint64_t acceptedPositions = 0;
    };

    // Callers must hold mutex_.
    void expireInactivePlayers(std::int64_t now);
    void advancePlatforms(std::int64_t now);
    void removePlayer(std::unordered_map<PlayerId, ActivePlayer>::iterator player);
    WorldSnapshot buildSnapshot() const;
    WorldStateSnapshot buildWorldState() const;
    SpawnPoint chooseSpawn(PlayerId id) const;

    const TimeSource& clock_;
    ServerConfig config_;
    mutable std::mutex mutex_;
    PlayerId nextPlayerId_ = 1;
    std::uint64_t serverTick_ = 0;
    std::uint64_t worldRevision_ = 0;
    std::int64_t lastPlatformUpdate_ = 0;
    // Clock tics the World has been advanced through, for FrameTime::gameTimeUs.
    std::int64_t worldElapsedTics_ = 0;

    World world_;
    std::unordered_map<PlayerId, ActivePlayer> players_;
    std::unordered_map<SessionToken, PlayerId> playerIdsByToken_;
    // Platform objects in config order, which is snapshot order. Never
    // destroyed, so the pointers live as long as the server.
    std::vector<GameObject*> platforms_;
};

} // namespace Network
