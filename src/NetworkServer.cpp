#include "NetworkServer.hpp"

#include "Components.hpp"
#include "FrameTime.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Network {
namespace {

float pathLengthOf(const PlatformPath& path)
{
    const float dx = path.endX - path.startX;
    const float dy = path.endY - path.startY;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

NetworkServer::NetworkServer(const TimeSource& clock, ServerConfig config)
    : clock_(clock),
      config_(std::move(config))
{
    if (config_.spawnPoints.empty() || config_.inactivityTimeoutTics <= 0) {
        throw std::invalid_argument("NetworkServer configuration is invalid");
    }

    for (const SpawnPoint& spawn : config_.spawnPoints) {
        if (!std::isfinite(spawn.x) || !std::isfinite(spawn.y)) {
            throw std::invalid_argument("NetworkServer spawn point must be finite");
        }
    }

    platforms_.reserve(config_.platforms.size());
    for (const PlatformPath& path : config_.platforms) {
        if (path.id == 0 || !std::isfinite(path.startX) || !std::isfinite(path.startY) ||
            !std::isfinite(path.endX) || !std::isfinite(path.endY) || !std::isfinite(path.speed) ||
            path.speed <= 0.0F || !std::isfinite(path.width) || !std::isfinite(path.height) ||
            path.width <= 0.0F || path.height <= 0.0F) {
            throw std::invalid_argument("NetworkServer platform path is invalid");
        }

        if (pathLengthOf(path) <= 0.0F) {
            throw std::invalid_argument("NetworkServer platform path must have non-zero length");
        }

        // The same moving platform a client builds: a PathMover drives it,
        // starting at the path's first point.
        GameObject& platform = world_.create("platform");
        platform.add<PathMover>(PathMover::Linear{path.startX, path.startY, path.endX, path.endY}, path.speed);
        Transform& transform = *platform.get<Transform>();
        transform.width = path.width;
        transform.height = path.height;
        platform.add<NetworkIdentity>(NetworkIdentity::Kind::Platform, path.id);
        platform.setActive(true);
        platforms_.push_back(&platform);
    }

    // Platforms run from the moment the server exists.
    lastPlatformUpdate_ = clock_.now();
}

void NetworkServer::update()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t now = clock_.now();
    advancePlatforms(now);
    expireInactivePlayers(now);
}

Message NetworkServer::handle(const Message& message)
{
    const std::lock_guard<std::mutex> lock(mutex_);

    const std::int64_t now = clock_.now();
    expireInactivePlayers(now);

    Request request;
    std::string error;
    if (!decodeRequest(message, request, error)) {
        return encodeError(error);
    }

    if (request.type == RequestType::GetWorld) {
        return encodeWorldState(buildWorldState());
    }

    if (request.type == RequestType::Join) {
        const auto existingId = playerIdsByToken_.find(request.sessionToken);
        if (existingId != playerIdsByToken_.end()) {
            const auto existingPlayer = players_.find(existingId->second);
            if (existingPlayer != players_.end()) {
                existingPlayer->second.lastHeard = now;
                return encodeWelcome(existingPlayer->first, buildSnapshot());
            }
            playerIdsByToken_.erase(existingId);
        }

        if (config_.maxPlayers != 0 && players_.size() >= config_.maxPlayers) {
            return encodeError("server is full");
        }

        const PlayerId id = nextPlayerId_++;
        const SpawnPoint spawn = chooseSpawn(id);

        GameObject& object = world_.create("player");
        object.add<Transform>(spawn.x, spawn.y, 0.0F, 0.0F);
        object.add<NetworkIdentity>(NetworkIdentity::Kind::Player, id);
        object.setActive(true);

        ActivePlayer player;
        player.object = &object;
        // Stored once at JOIN and relayed in every snapshot afterwards.
        player.name = request.playerName;
        player.sessionToken = request.sessionToken;
        player.lastHeard = now;
        players_.emplace(id, std::move(player));
        playerIdsByToken_.emplace(request.sessionToken, id);
        ++serverTick_;
        return encodeWelcome(id, buildSnapshot());
    }

    const auto player = players_.find(request.playerId);
    if (player == players_.end()) {
        return encodeError("unknown player ID");
    }

    if (player->second.sessionToken != request.sessionToken) {
        return encodeError("session token does not own this player");
    }

    if (request.type == RequestType::Leave) {
        removePlayer(player);
        ++serverTick_;
        return encodeGoodbye();
    }

    if (request.position.sequence <= player->second.lastSequence) {
        return encodeError("position sequence must increase");
    }

    Transform& transform = *player->second.object->get<Transform>();
    transform.x = request.position.x;
    transform.y = request.position.y;
    // Whatever this game chose to say about its player. Stored and relayed
    // verbatim: the server has no idea what is in it, which is exactly why a
    // game can change what it sends without the server being touched.
    player->second.data = std::move(request.position.data);
    player->second.lastSequence = request.position.sequence;
    player->second.lastHeard = now;
    ++player->second.acceptedPositions;
    ++serverTick_;
    return encodeSnapshot(buildSnapshot());
}

WorldSnapshot NetworkServer::snapshot() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return buildSnapshot();
}

std::size_t NetworkServer::playerCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return players_.size();
}

bool NetworkServer::hasSession(const SessionToken& sessionToken) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return playerIdsByToken_.count(sessionToken) != 0;
}

std::vector<PlayerTraffic> NetworkServer::traffic() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PlayerTraffic> result;
    result.reserve(players_.size());
    for (const auto& entry : players_) {
        result.push_back({entry.first, entry.second.name, entry.second.acceptedPositions});
    }
    std::sort(result.begin(), result.end(),
              [](const PlayerTraffic& a, const PlayerTraffic& b) { return a.id < b.id; });
    return result;
}

void NetworkServer::expireInactivePlayers(std::int64_t now)
{
    for (auto player = players_.begin(); player != players_.end();) {
        if (now - player->second.lastHeard >= config_.inactivityTimeoutTics) {
            auto expired = player++;
            removePlayer(expired);
            ++serverTick_;
        } else {
            ++player;
        }
    }
}

void NetworkServer::removePlayer(std::unordered_map<PlayerId, ActivePlayer>::iterator player)
{
    // The player's object leaves the scene with its connection.
    world_.destroy(player->second.object->id());
    world_.flushDestroyed();
    playerIdsByToken_.erase(player->second.sessionToken);
    players_.erase(player);
}

void NetworkServer::advancePlatforms(std::int64_t now)
{
    const std::int64_t deltaTics = now - lastPlatformUpdate_;
    if (deltaTics <= 0) {
        return;
    }
    lastPlatformUpdate_ = now;

    if (platforms_.empty()) {
        return;
    }

    // TimeSource for the demo/server is real nanoseconds; ManualClock tests use
    // the same unit convention (advance in ns). Speeds are units per second.
    worldElapsedTics_ += deltaTics;
    const FrameTime time{
        static_cast<double>(deltaTics) / static_cast<double>(kNsPerSec),
        worldElapsedTics_ / (kNsPerSec / 1'000'000),
    };
    world_.update(time);

    ++serverTick_;
    ++worldRevision_;
}

WorldSnapshot NetworkServer::buildSnapshot() const
{
    WorldSnapshot result;
    result.serverTick = serverTick_;
    result.players.reserve(players_.size());
    for (const auto& entry : players_) {
        const Transform& transform = *entry.second.object->get<Transform>();
        result.players.push_back({entry.first, entry.second.name, transform.x, transform.y, entry.second.data});
    }
    std::sort(result.players.begin(), result.players.end(),
              [](const PlayerState& left, const PlayerState& right) { return left.id < right.id; });

    result.platforms = buildWorldState().platforms;
    return result;
}

WorldStateSnapshot NetworkServer::buildWorldState() const
{
    WorldStateSnapshot result;
    result.worldRevision = worldRevision_;
    result.platforms.reserve(platforms_.size());
    for (const GameObject* platform : platforms_) {
        const Transform& transform = *platform->get<Transform>();
        result.platforms.push_back({platform->get<NetworkIdentity>()->id, transform.x, transform.y,
                                    transform.width, transform.height});
    }
    return result;
}

SpawnPoint NetworkServer::chooseSpawn(PlayerId id) const
{
    const std::size_t count = config_.spawnPoints.size();
    const std::size_t firstIndex = static_cast<std::size_t>(id - 1) % count;

    // Start at the usual rotating point, but prefer an empty configured spawn
    // so join/leave churn cannot immediately stack two active players.
    for (std::size_t offset = 0; offset < count; ++offset) {
        const SpawnPoint& candidate = config_.spawnPoints[(firstIndex + offset) % count];
        const bool occupied = std::any_of(players_.begin(), players_.end(), [&](const std::pair<const PlayerId, ActivePlayer>& entry) {
            const Transform& transform = *entry.second.object->get<Transform>();
            return transform.x == candidate.x && transform.y == candidate.y;
        });
        if (!occupied) {
            return candidate;
        }
    }
    return config_.spawnPoints[firstIndex];
}

} // namespace Network
