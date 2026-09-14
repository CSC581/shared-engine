#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Portable values exchanged by the networking module. Messages are a vector
// of text fields so they never depend on C++ struct layout or byte order.
namespace Network {

constexpr int protocolVersion = 3;

using PlayerId = std::uint32_t;
using SessionToken = std::string;
using Message = std::vector<std::string>;

struct PositionUpdate {
    float x = 0.0F;
    float y = 0.0F;
    std::uint64_t sequence = 0;
};

struct PlayerState {
    PlayerId id = 0;
    float x = 0.0F;
    float y = 0.0F;
};

struct WorldSnapshot {
    // Revision of stored positions and membership, not elapsed simulation time.
    std::uint64_t serverTick = 0;
    std::vector<PlayerState> players;
};

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error,
};

enum class RequestType {
    Join,
    Position,
    Leave,
};

struct Request {
    RequestType type = RequestType::Join;
    PlayerId playerId = 0;
    SessionToken sessionToken;
    PositionUpdate position{};
};

enum class ReplyType {
    Welcome,
    Snapshot,
    Error,
    Goodbye,
};

struct Reply {
    ReplyType type = ReplyType::Error;
    PlayerId playerId = 0;
    WorldSnapshot snapshot{};
    std::string error;
};

Message encodeJoin(const SessionToken& sessionToken);
Message encodePosition(PlayerId playerId, const SessionToken& sessionToken, const PositionUpdate& position);
Message encodeLeave(PlayerId playerId, const SessionToken& sessionToken);
bool decodeRequest(const Message& message, Request& request, std::string& error);

Message encodeWelcome(PlayerId playerId, const WorldSnapshot& snapshot);
Message encodeSnapshot(const WorldSnapshot& snapshot);
Message encodeError(const std::string& error);
Message encodeGoodbye();
bool decodeReply(const Message& message, Reply& reply, std::string& error);

} // namespace Network
