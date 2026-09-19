#include "NetworkServer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Network {

NetworkServer::NetworkServer(const TimeSource& clock, ServerConfig config)
    : clock_(clock),
      config_(config)
{
    if (config_.spawnPoints.empty() || config_.maxPlayers == 0 || config_.inactivityTimeoutTics <= 0) {
        throw std::invalid_argument("NetworkServer configuration is invalid");
    }

    for (const SpawnPoint& spawn : config_.spawnPoints) {
        if (!std::isfinite(spawn.x) || !std::isfinite(spawn.y)) {
            throw std::invalid_argument("NetworkServer spawn point must be finite");
        }
    }
}

void NetworkServer::update()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    expireInactivePlayers(clock_.now());
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
        players_.emplace(id, ActivePlayer{spawnPlayer(id), request.sessionToken, {}, now});
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
