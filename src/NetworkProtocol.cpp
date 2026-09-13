#include "NetworkProtocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Network {
namespace {

bool parseUnsigned(const std::string& text, std::uint64_t& value)
{
    try {
        std::size_t parsed = 0;
        value = std::stoull(text, &parsed);
        return parsed == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool parseInt(const std::string& text, int& value)
{
    try {
        std::size_t parsed = 0;
        value = std::stoi(text, &parsed);
        return parsed == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool parseFloat(const std::string& text, float& value)
{
    try {
        std::size_t parsed = 0;
        value = std::stof(text, &parsed);
        return parsed == text.size() && std::isfinite(value);
    } catch (const std::exception&) {
        return false;
    }
}

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

    constexpr std::size_t playerFieldCount = 3;
    if (playerCount > (std::numeric_limits<std::size_t>::max() - index - 2) / playerFieldCount ||
        message.size() != index + 2 + static_cast<std::size_t>(playerCount) * playerFieldCount) {
        error = "invalid snapshot player count";
        return false;
    }

    snapshot.players.clear();
    snapshot.players.reserve(static_cast<std::size_t>(playerCount));

    index += 2;
    for (std::uint64_t i = 0; i < playerCount; ++i) {
        std::uint64_t id = 0;
        PlayerState player;
        if (!parseUnsigned(message[index], id) || id == 0 || id > std::numeric_limits<PlayerId>::max() ||
            !parseFloat(message[index + 1], player.x) || !parseFloat(message[index + 2], player.y)) {
            error = "invalid player in snapshot";
            return false;
        }

        player.id = static_cast<PlayerId>(id);
        snapshot.players.push_back(player);
        index += playerFieldCount;
    }

    return true;
}

void appendSnapshot(Message& message, const WorldSnapshot& snapshot)
{
    message.push_back(std::to_string(snapshot.serverTick));
    message.push_back(std::to_string(snapshot.players.size()));

    for (const PlayerState& player : snapshot.players) {
        message.push_back(std::to_string(player.id));
        message.push_back(std::to_string(player.x));
        message.push_back(std::to_string(player.y));
    }
}

} // namespace

Message encodeJoin(const SessionToken& sessionToken)
{
    return {std::to_string(protocolVersion), "JOIN", sessionToken};
}

Message encodeInput(PlayerId playerId, const SessionToken& sessionToken, const MovementInput& input)
{
    return {std::to_string(protocolVersion), "INPUT", std::to_string(playerId), sessionToken,
            std::to_string(input.sequence), std::to_string(input.horizontal), std::to_string(input.vertical)};
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
        if (message.size() != 3 || !isValidSessionToken(message[2])) {
            error = "JOIN requires a valid session token";
            return false;
        }
        request = {};
        request.type = RequestType::Join;
        request.sessionToken = message[2];
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

    if (message[1] != "INPUT" || message.size() != 7) {
        error = "unknown request command";
        return false;
    }

    std::uint64_t sequence = 0;
    if (!parseUnsigned(message[4], sequence) || !parseInt(message[5], request.input.horizontal) ||
        !parseInt(message[6], request.input.vertical) || request.input.horizontal < -1 ||
        request.input.horizontal > 1 || request.input.vertical < -1 || request.input.vertical > 1) {
        error = "invalid movement input";
        return false;
    }

    request.type = RequestType::Input;
    request.input.sequence = sequence;
    return true;
}

Message encodeWelcome(PlayerId playerId, const WorldSnapshot& snapshot)
{
    Message message{std::to_string(protocolVersion), "WELCOME", std::to_string(playerId)};
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
        if (message.size() < 5 || !parseUnsigned(message[2], playerId) || playerId == 0 ||
            playerId > std::numeric_limits<PlayerId>::max()) {
            error = "invalid WELCOME reply";
            return false;
        }
        reply.type = ReplyType::Welcome;
        reply.playerId = static_cast<PlayerId>(playerId);
        snapshotIndex = 3;
    } else if (message[1] == "SNAPSHOT") {
        reply.type = ReplyType::Snapshot;
    } else {
        error = "unknown reply command";
        return false;
    }

    return parseSnapshot(message, snapshotIndex, reply.snapshot, error);
}

} // namespace Network
