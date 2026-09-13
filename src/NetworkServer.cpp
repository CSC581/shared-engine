#include "NetworkServer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Network {

NetworkServer::NetworkServer(const TimeSource& clock, ServerConfig config)
    : clock_(clock),
      config_(config),
      lastUpdate_(clock.now())
{
    if (config_.arenaWidth <= config_.playerSize || config_.arenaHeight <= config_.playerSize ||
        config_.playerSize <= 0.0F || config_.playerSpeed < 0.0F || config_.spawnPoints.empty() || config_.maxPlayers == 0 ||
        config_.ticsPerSecond <= 0 || config_.inactivityTimeoutTics <= 0) {
        throw std::invalid_argument("NetworkServer configuration is invalid");
    }

    for (const SpawnPoint& spawn : config_.spawnPoints) {
        if (spawn.x < 0.0F || spawn.y < 0.0F || spawn.x > config_.arenaWidth - config_.playerSize ||
            spawn.y > config_.arenaHeight - config_.playerSize) {
            throw std::invalid_argument("NetworkServer spawn point is outside the arena");
        }
    }
}

void NetworkServer::update()
{
    const std::int64_t now = clock_.now();
    const std::int64_t elapsed = std::max<std::int64_t>(0, now - lastUpdate_);
    lastUpdate_ = now;

    if (elapsed > 0) {
        const float elapsedSeconds = static_cast<float>(elapsed) / static_cast<float>(config_.ticsPerSecond);
        for (auto& entry : players_) {
            ActivePlayer& player = entry.second;
            float horizontal = static_cast<float>(player.input.horizontal);
            float vertical = static_cast<float>(player.input.vertical);
            const float length = std::sqrt(horizontal * horizontal + vertical * vertical);
            if (length > 1.0F) {
                horizontal /= length;
                vertical /= length;
            }

            player.state.x = std::clamp(player.state.x + horizontal * config_.playerSpeed * elapsedSeconds,
                                        0.0F, config_.arenaWidth - config_.playerSize);
            player.state.y = std::clamp(player.state.y + vertical * config_.playerSpeed * elapsedSeconds,
                                        0.0F, config_.arenaHeight - config_.playerSize);
        }
        ++serverTick_;
    }

    expireInactivePlayers(now);
}

Message NetworkServer::handle(const Message& message)
{
    update();

    Request request;
    std::string error;
    if (!decodeRequest(message, request, error)) {
        return encodeError(error);
    }

    const std::int64_t now = clock_.now();
    if (request.type == RequestType::Join) {
        const auto existingId = playerIdsByToken_.find(request.sessionToken);
        if (existingId != playerIdsByToken_.end()) {
            const auto existingPlayer = players_.find(existingId->second);
            if (existingPlayer != players_.end()) {
                existingPlayer->second.lastHeard = now;
                return encodeWelcome(existingPlayer->first, snapshotLocked());
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
        return encodeWelcome(id, snapshotLocked());
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

    if (request.input.sequence <= player->second.input.sequence) {
        return encodeError("input sequence must increase");
    }

    player->second.input = request.input;
    player->second.lastHeard = now;
    return encodeSnapshot(snapshotLocked());
}

WorldSnapshot NetworkServer::snapshot() const
{
    return snapshotLocked();
}

std::size_t NetworkServer::playerCount() const
{
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

WorldSnapshot NetworkServer::snapshotLocked() const
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
    const std::size_t index = static_cast<std::size_t>(id - 1);
    const SpawnPoint spawn = config_.spawnPoints[index % config_.spawnPoints.size()];

    PlayerState player;
    player.id = id;
    player.x = spawn.x;
    player.y = spawn.y;
    return player;
}

} // namespace Network
