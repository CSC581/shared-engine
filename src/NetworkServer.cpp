#include "NetworkServer.hpp"

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
    if (config_.spawnPoints.empty() || config_.maxPlayers == 0 || config_.inactivityTimeoutTics <= 0) {
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

        const float length = pathLengthOf(path);
        if (length <= 0.0F) {
            throw std::invalid_argument("NetworkServer platform path must have non-zero length");
        }

        ActivePlatform platform;
        platform.path = path;
        platform.pathLength = length;
        platform.distance = 0.0F;
        platform.velocity = path.speed;
        platform.state.id = path.id;
        platform.state.x = path.startX;
        platform.state.y = path.startY;
        platform.state.width = path.width;
        platform.state.height = path.height;
        platforms_.push_back(platform);
    }
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
    advancePlatforms(now);
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

        if (players_.size() >= config_.maxPlayers) {
            return encodeError("server is full");
        }

        const PlayerId id = nextPlayerId_++;
        PlayerState spawned = spawnPlayer(id);
        // Stored once at JOIN and relayed in every snapshot afterwards.
        spawned.name = request.playerName;
        players_.emplace(id, ActivePlayer{std::move(spawned), request.sessionToken, {}, now});
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
        playerIdsByToken_.erase(player->second.sessionToken);
        players_.erase(player);
        ++serverTick_;
        return encodeGoodbye();
    }

    if (request.position.sequence <= player->second.lastSequence) {
        return encodeError("position sequence must increase");
    }

    player->second.state.x = request.position.x;
    player->second.state.y = request.position.y;
    // Whatever this game chose to say about its player. Stored and relayed
    // verbatim: the server has no idea what is in it, which is exactly why a
    // game can change what it sends without the server being touched.
    player->second.state.data = std::move(request.position.data);
    player->second.lastSequence = request.position.sequence;
    player->second.lastHeard = now;
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

void NetworkServer::expireInactivePlayers(std::int64_t now)
{
    for (auto player = players_.begin(); player != players_.end();) {
        if (now - player->second.lastHeard >= config_.inactivityTimeoutTics) {
            playerIdsByToken_.erase(player->second.sessionToken);
            player = players_.erase(player);
            ++serverTick_;
        } else {
            ++player;
        }
    }
}

void NetworkServer::advancePlatforms(std::int64_t now)
{
    if (platforms_.empty()) {
        return;
    }

    if (!platformClockStarted_) {
        lastPlatformUpdate_ = now;
        platformClockStarted_ = true;
        return;
    }

    const std::int64_t deltaTics = now - lastPlatformUpdate_;
    if (deltaTics <= 0) {
        return;
    }
    lastPlatformUpdate_ = now;

    // TimeSource for the demo/server is real nanoseconds; ManualClock tests use
    // the same unit convention (advance in ns). Speed is units per second.
    const float dtSeconds = static_cast<float>(deltaTics) / static_cast<float>(kNsPerSec);
    bool moved = false;

    for (ActivePlatform& platform : platforms_) {
        float travel = std::fabs(platform.velocity) * dtSeconds;
        if (travel == 0.0F) {
            continue;
        }
        moved = true;

        while (travel > 0.0F) {
            if (platform.velocity > 0.0F) {
                const float room = platform.pathLength - platform.distance;
                if (room <= 0.0F) {
                    platform.velocity = -platform.path.speed;
                    continue;
                }
                const float step = std::min(travel, room);
                platform.distance += step;
                travel -= step;
            } else {
                const float room = platform.distance;
                if (room <= 0.0F) {
                    platform.velocity = platform.path.speed;
                    continue;
                }
                const float step = std::min(travel, room);
                platform.distance -= step;
                travel -= step;
            }
        }

        const float t = platform.distance / platform.pathLength;
        platform.state.x = platform.path.startX + (platform.path.endX - platform.path.startX) * t;
        platform.state.y = platform.path.startY + (platform.path.endY - platform.path.startY) * t;
    }

    if (moved) {
        ++serverTick_;
        ++worldRevision_;
    }
}

WorldSnapshot NetworkServer::buildSnapshot() const
{
    WorldSnapshot result;
    result.serverTick = serverTick_;
    result.players.reserve(players_.size());
    for (const auto& entry : players_) {
        result.players.push_back(entry.second.state);
    }
    std::sort(result.players.begin(), result.players.end(),
              [](const PlayerState& left, const PlayerState& right) { return left.id < right.id; });

    result.platforms.reserve(platforms_.size());
    for (const ActivePlatform& platform : platforms_) {
        result.platforms.push_back(platform.state);
    }
    return result;
}

WorldStateSnapshot NetworkServer::buildWorldState() const
{
    WorldStateSnapshot result;
    result.worldRevision = worldRevision_;
    result.platforms.reserve(platforms_.size());
    for (const ActivePlatform& platform : platforms_) {
        result.platforms.push_back(platform.state);
    }
    return result;
}

PlayerState NetworkServer::spawnPlayer(PlayerId id) const
{
    const std::size_t count = config_.spawnPoints.size();
    const std::size_t firstIndex = static_cast<std::size_t>(id - 1) % count;

    // Start at the usual rotating point, but prefer an empty configured spawn
    // so join/leave churn cannot immediately stack two active players.
    const SpawnPoint* selected = &config_.spawnPoints[firstIndex];
    for (std::size_t offset = 0; offset < count; ++offset) {
        const SpawnPoint& candidate = config_.spawnPoints[(firstIndex + offset) % count];
        const bool occupied = std::any_of(players_.begin(), players_.end(), [&](const auto& entry) {
            return entry.second.state.x == candidate.x && entry.second.state.y == candidate.y;
        });
        if (!occupied) {
            selected = &candidate;
            break;
        }
    }

    PlayerState player;
    player.id = id;
    player.x = selected->x;
    player.y = selected->y;
    return player;
}

} // namespace Network
