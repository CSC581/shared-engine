#include "NetworkProtocol.hpp"

#include "Endpoint.hpp"
#include "WireFormat.hpp"

#include <utility>

#include <algorithm>
#include <cctype>
#include <limits>

namespace Network {
namespace {

// Field encoding is shared with the peer protocol; see WireFormat.hpp.
using Net::formatFloat;
using Net::parseFloat;
using Net::parseUnsigned;

bool parseVersion(const Message& message, std::string& error)
{
    std::uint64_t version = 0;
    if (message.size() < 2 || !parseUnsigned(message[0], version)) {
        error = "message must start with a protocol version and command";
        return false;
    }

    if (version != protocolVersion) {
        error = "unsupported protocol version";
        return false;
    }

    return true;
}

bool isValidSessionToken(const std::string& token)
{
    if (token.size() != 32) {
        return false;
    }

    return std::all_of(token.begin(), token.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

bool parseSnapshot(const Message& message, std::size_t index, WorldSnapshot& snapshot, std::string& error)
{
    std::uint64_t playerCount = 0;
    if (message.size() < index + 2 || !parseUnsigned(message[index], snapshot.serverTick) ||
        !parseUnsigned(message[index + 1], playerCount)) {
        error = "invalid snapshot header";
        return false;
    }

    constexpr std::size_t platformFieldCount = 5;
    if (playerCount > message.size()) {
        error = "invalid snapshot player count";
        return false;
    }

    snapshot.players.clear();
    snapshot.players.reserve(static_cast<std::size_t>(playerCount));

    // Players are variable length now that each carries a name and whatever
    // game-defined fields it has, so the reader walks them rather than
    // indexing by a fixed stride.
    index += 2;
    for (std::uint64_t i = 0; i < playerCount; ++i) {
        std::uint64_t id = 0;
        PlayerState player;
        if (message.size() < index + 5 || !parseUnsigned(message[index], id) || id == 0 ||
            id > std::numeric_limits<PlayerId>::max() || !Net::isValidName(message[index + 1]) ||
            !parseFloat(message[index + 2], player.x) || !parseFloat(message[index + 3], player.y) ||
            !Net::isValidPlayerData(message[index + 4])) {
            error = "invalid player in snapshot";
            return false;
        }

        player.id = static_cast<PlayerId>(id);
        player.name = message[index + 1];
        player.data = message[index + 4];
        index += 5;
        snapshot.players.push_back(std::move(player));
    }

    if (index >= message.size()) {
        error = "invalid snapshot platform header";
        return false;
    }

    std::uint64_t platformCount = 0;
    if (!parseUnsigned(message[index], platformCount)) {
        error = "invalid snapshot platform count";
        return false;
    }
    ++index;

    if (platformCount > (std::numeric_limits<std::size_t>::max() - index) / platformFieldCount ||
        message.size() != index + static_cast<std::size_t>(platformCount) * platformFieldCount) {
        error = "invalid snapshot platform count";
        return false;
    }

    snapshot.platforms.clear();
    snapshot.platforms.reserve(static_cast<std::size_t>(platformCount));
    for (std::uint64_t i = 0; i < platformCount; ++i) {
        std::uint64_t id = 0;
        PlatformState platform;
        if (!parseUnsigned(message[index], id) || id == 0 || id > std::numeric_limits<std::uint32_t>::max() ||
            !parseFloat(message[index + 1], platform.x) || !parseFloat(message[index + 2], platform.y) ||
            !parseFloat(message[index + 3], platform.width) || !parseFloat(message[index + 4], platform.height) ||
            platform.width <= 0.0F || platform.height <= 0.0F) {
            error = "invalid platform in snapshot";
            return false;
        }

        platform.id = static_cast<std::uint32_t>(id);
        snapshot.platforms.push_back(platform);
        index += platformFieldCount;
    }

    return true;
}

void appendSnapshot(Message& message, const WorldSnapshot& snapshot)
{
    message.push_back(std::to_string(snapshot.serverTick));
    message.push_back(std::to_string(snapshot.players.size()));

    for (const PlayerState& player : snapshot.players) {
        message.push_back(std::to_string(player.id));
        message.push_back(player.name);
        message.push_back(formatFloat(player.x));
        message.push_back(formatFloat(player.y));
        message.push_back(player.data);
    }

    message.push_back(std::to_string(snapshot.platforms.size()));
    for (const PlatformState& platform : snapshot.platforms) {
        message.push_back(std::to_string(platform.id));
        message.push_back(formatFloat(platform.x));
        message.push_back(formatFloat(platform.y));
        message.push_back(formatFloat(platform.width));
        message.push_back(formatFloat(platform.height));
    }
}

} // namespace

Message encodeJoin(const SessionToken& sessionToken, const std::string& playerName)
{
    return {std::to_string(protocolVersion), "JOIN", sessionToken, playerName};
}

Message encodePosition(PlayerId playerId, const SessionToken& sessionToken, const PositionUpdate& position)
{
    return {std::to_string(protocolVersion), "POSITION",       std::to_string(playerId),
            sessionToken,                    std::to_string(position.sequence),
            formatFloat(position.x),         formatFloat(position.y),
            position.data};
}

Message encodeLeave(PlayerId playerId, const SessionToken& sessionToken)
{
    return {std::to_string(protocolVersion), "LEAVE", std::to_string(playerId), sessionToken};
}

bool decodeRequest(const Message& message, Request& request, std::string& error)
{
    if (!parseVersion(message, error)) {
        return false;
    }

    if (message[1] == "JOIN") {
        if (message.size() != 4 || !isValidSessionToken(message[2]) || !Net::isValidName(message[3])) {
            error = "JOIN requires a valid session token and name";
            return false;
        }
        request = {};
        request.type = RequestType::Join;
        request.sessionToken = message[2];
        // Sent once, at JOIN, rather than on every position update: a name
        // does not change and there is no sense paying for it every frame.
        request.playerName = message[3];
        return true;
    }

    std::uint64_t playerId = 0;
    if (message.size() < 4 || !parseUnsigned(message[2], playerId) || playerId == 0 ||
        playerId > std::numeric_limits<PlayerId>::max()) {
        error = "invalid player ID";
        return false;
    }

    request = {};
    request.playerId = static_cast<PlayerId>(playerId);
    if (!isValidSessionToken(message[3])) {
        error = "invalid session token";
        return false;
    }
    request.sessionToken = message[3];

    if (message[1] == "LEAVE") {
        if (message.size() != 4) {
            error = "LEAVE requires a player ID and session token";
            return false;
        }
        request.type = RequestType::Leave;
        return true;
    }

    if (message[1] != "POSITION" || message.size() != 8) {
        error = "unknown request command";
        return false;
    }

    std::uint64_t sequence = 0;
    if (!parseUnsigned(message[4], sequence) || sequence == 0 ||
        !parseFloat(message[5], request.position.x) || !parseFloat(message[6], request.position.y)) {
        error = "invalid position update";
        return false;
    }

    if (!Net::isValidPlayerData(message[7])) {
        error = "player data is too large";
        return false;
    }
    request.position.data = message[7];

    request.type = RequestType::Position;
    request.position.sequence = sequence;
    return true;
}

std::string rewriteTcpEndpointHost(const std::string& endpoint, const std::string& host)
{
    // Kept as part of the Network API; the implementation is shared with the
    // peer-to-peer module through Endpoint.hpp so there is only ever one.
    return Net::rewriteTcpEndpointHost(endpoint, host);
}

Message encodeWelcome(PlayerId playerId, const WorldSnapshot& snapshot, const std::string& sessionEndpoint)
{
    Message message{std::to_string(protocolVersion), "WELCOME", std::to_string(playerId), sessionEndpoint};
    appendSnapshot(message, snapshot);
    return message;
}

Message encodeSnapshot(const WorldSnapshot& snapshot)
{
    Message message{std::to_string(protocolVersion), "SNAPSHOT"};
    appendSnapshot(message, snapshot);
    return message;
}

Message encodeError(const std::string& error)
{
    return {std::to_string(protocolVersion), "ERROR", error};
}

Message encodeGoodbye()
{
    return {std::to_string(protocolVersion), "GOODBYE"};
}

bool decodeReply(const Message& message, Reply& reply, std::string& error)
{
    if (!parseVersion(message, error)) {
        return false;
    }

    reply = {};
    if (message[1] == "ERROR") {
        if (message.size() != 3) {
            error = "invalid ERROR reply";
            return false;
        }
        reply.type = ReplyType::Error;
        reply.error = message[2];
        return true;
    }

    if (message[1] == "GOODBYE") {
        if (message.size() != 2) {
            error = "invalid GOODBYE reply";
            return false;
        }
        reply.type = ReplyType::Goodbye;
        return true;
    }

    std::size_t snapshotIndex = 2;
    if (message[1] == "WELCOME") {
        std::uint64_t playerId = 0;
        // [version, WELCOME, playerId, sessionEndpoint, ...snapshot...]
        // Snapshot needs at least tick, playerCount, platformCount.
        if (message.size() < 7 || !parseUnsigned(message[2], playerId) || playerId == 0 ||
            playerId > std::numeric_limits<PlayerId>::max()) {
            error = "invalid WELCOME reply";
            return false;
        }
        reply.type = ReplyType::Welcome;
        reply.playerId = static_cast<PlayerId>(playerId);
        reply.sessionEndpoint = message[3];
        snapshotIndex = 4;
    } else if (message[1] == "SNAPSHOT") {
        reply.type = ReplyType::Snapshot;
    } else {
        error = "unknown reply command";
        return false;
    }

    return parseSnapshot(message, snapshotIndex, reply.snapshot, error);
}

} // namespace Network
