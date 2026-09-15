#include "NetworkServer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace Network {
namespace {

constexpr int stateFileVersion = 1;

bool parseUnsigned(const std::string& text, std::uint64_t& value)
{
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return false;
    }

    try {
        std::size_t consumed = 0;
        value = std::stoull(text, &consumed);
        return consumed == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool isValidToken(const SessionToken& token)
{
    return token.size() == 32 && std::all_of(token.begin(), token.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

std::string formatFloat(float value)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return output.str();
}

[[noreturn]] void stateFileError(const std::string& path, const std::string& reason)
{
    throw std::runtime_error("Invalid network server state file '" + path + "': " + reason);
}

} // namespace

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

    loadState();
}

void NetworkServer::update()
{
    if (expireInactivePlayers(clock_.now())) {
        saveState();
    }
}

Message NetworkServer::handle(const Message& message)
{
    const std::int64_t now = clock_.now();
    if (expireInactivePlayers(now)) {
        saveState();
    }

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
        saveState();
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
        saveState();
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
    saveState();
    return encodeSnapshot(buildSnapshot());
}

WorldSnapshot NetworkServer::snapshot() const
{
    return buildSnapshot();
}

std::size_t NetworkServer::playerCount() const
{
    return players_.size();
}

bool NetworkServer::expireInactivePlayers(std::int64_t now)
{
    bool removedPlayer = false;
    for (auto player = players_.begin(); player != players_.end();) {
        if (now - player->second.lastHeard >= config_.inactivityTimeoutTics) {
            playerIdsByToken_.erase(player->second.sessionToken);
            player = players_.erase(player);
            ++serverTick_;
            removedPlayer = true;
        } else {
            ++player;
        }
    }
    return removedPlayer;
}

void NetworkServer::loadState()
{
    if (config_.stateFilePath.empty()) {
        return;
    }

    const std::filesystem::path path(config_.stateFilePath);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        throw std::runtime_error("Cannot check network server state file '" + config_.stateFilePath + "': " + error.message());
    }
    if (!exists) {
        return;
    }

    std::ifstream input(path);
    input.imbue(std::locale::classic());
    if (!input) {
        throw std::runtime_error("Cannot open network server state file '" + config_.stateFilePath + "'");
    }

    std::string line;
    if (!std::getline(input, line) || line != "NETWORK_SERVER_STATE " + std::to_string(stateFileVersion)) {
        stateFileError(config_.stateFilePath, "missing or unsupported header");
    }

    bool readNextPlayerId = false;
    bool readServerTick = false;
    std::uint64_t loadedNextPlayerId = 0;
    std::uint64_t loadedServerTick = 0;
    PlayerId largestPlayerId = 0;
    const std::int64_t restartedAt = clock_.now();

    while (std::getline(input, line)) {
        if (line.empty()) {
            stateFileError(config_.stateFilePath, "unexpected blank line");
        }

        std::istringstream fields(line);
        fields.imbue(std::locale::classic());
        std::string tag;
        std::string value;
        std::string extra;
        fields >> tag;

        if (tag == "NEXT_PLAYER_ID" || tag == "SERVER_TICK") {
            if (!(fields >> value) || (fields >> extra)) {
                stateFileError(config_.stateFilePath, "invalid " + tag + " record");
            }
            std::uint64_t parsedValue = 0;
            if (!parseUnsigned(value, parsedValue)) {
                stateFileError(config_.stateFilePath, "invalid " + tag + " value");
            }
            if (tag == "NEXT_PLAYER_ID") {
                if (readNextPlayerId || parsedValue == 0 || parsedValue > std::numeric_limits<PlayerId>::max()) {
                    stateFileError(config_.stateFilePath, "invalid NEXT_PLAYER_ID value");
                }
                loadedNextPlayerId = parsedValue;
                readNextPlayerId = true;
            } else {
                if (readServerTick) {
                    stateFileError(config_.stateFilePath, "duplicate SERVER_TICK record");
                }
                loadedServerTick = parsedValue;
                readServerTick = true;
            }
            continue;
        }

        if (tag != "PLAYER") {
            stateFileError(config_.stateFilePath, "unknown record");
        }

        std::string playerId;
        std::string sessionToken;
        std::string sequence;
        std::string x;
        std::string y;
        if (!(fields >> playerId >> sessionToken >> sequence >> x >> y) || (fields >> extra) || !isValidToken(sessionToken)) {
            stateFileError(config_.stateFilePath, "invalid PLAYER record");
        }

        Request request;
        std::string decodeError;
        if (!decodeRequest({std::to_string(protocolVersion), "POSITION", playerId, sessionToken, sequence, x, y},
                           request, decodeError)) {
            stateFileError(config_.stateFilePath, "invalid PLAYER record: " + decodeError);
        }
        if (players_.size() >= config_.maxPlayers || players_.find(request.playerId) != players_.end() ||
            playerIdsByToken_.find(sessionToken) != playerIdsByToken_.end()) {
            stateFileError(config_.stateFilePath, "duplicate player/token or too many players");
        }

        players_.emplace(request.playerId,
                         ActivePlayer{{request.playerId, request.position.x, request.position.y}, request.sessionToken,
                                      request.position.sequence, restartedAt});
        playerIdsByToken_.emplace(request.sessionToken, request.playerId);
        largestPlayerId = std::max(largestPlayerId, request.playerId);
    }

    if (!readNextPlayerId || !readServerTick || loadedNextPlayerId <= largestPlayerId) {
        stateFileError(config_.stateFilePath, "missing counters or NEXT_PLAYER_ID is not ahead of all players");
    }
    nextPlayerId_ = static_cast<PlayerId>(loadedNextPlayerId);
    serverTick_ = loadedServerTick;
}

void NetworkServer::saveState() const
{
    if (config_.stateFilePath.empty()) {
        return;
    }

    const std::filesystem::path path(config_.stateFilePath);
    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    std::ofstream output(temporaryPath, std::ios::trunc);
    output.imbue(std::locale::classic());
    if (!output) {
        throw std::runtime_error("Cannot write network server state file '" + temporaryPath.string() + "'");
    }

    output << "NETWORK_SERVER_STATE " << stateFileVersion << '\n';
    output << "NEXT_PLAYER_ID " << nextPlayerId_ << '\n';
    output << "SERVER_TICK " << serverTick_ << '\n';

    std::vector<const ActivePlayer*> orderedPlayers;
    orderedPlayers.reserve(players_.size());
    for (const auto& entry : players_) {
        orderedPlayers.push_back(&entry.second);
    }
    std::sort(orderedPlayers.begin(), orderedPlayers.end(), [](const ActivePlayer* left, const ActivePlayer* right) {
        return left->state.id < right->state.id;
    });
    for (const ActivePlayer* player : orderedPlayers) {
        output << "PLAYER " << player->state.id << ' ' << player->sessionToken << ' ' << player->lastSequence << ' '
               << formatFloat(player->state.x) << ' ' << formatFloat(player->state.y) << '\n';
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Cannot finish network server state file '" + temporaryPath.string() + "'");
    }

    std::error_code error;
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::filesystem::remove(temporaryPath);
        throw std::runtime_error("Cannot replace network server state file '" + config_.stateFilePath + "': " + error.message());
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
