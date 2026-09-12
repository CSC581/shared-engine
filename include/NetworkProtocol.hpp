#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Portable values exchanged by the networking module. Messages are a vector
// of text fields so they never depend on C++ struct layout or byte order.
namespace Network {

constexpr int protocolVersion = 1;

using PlayerId = std::uint32_t;
using Message = std::vector<std::string>;

struct MovementInput {
    int horizontal = 0;
    int vertical = 0;
    std::uint64_t sequence = 0;
};

struct PlayerState {
    PlayerId id = 0;
    float x = 0.0F;
    float y = 0.0F;
};

struct WorldSnapshot {
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
    Input,
    Leave,
};

struct Request {
    RequestType type = RequestType::Join;
    PlayerId playerId = 0;
    MovementInput input{};
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

Message encodeJoin();
Message encodeInput(PlayerId playerId, const MovementInput& input);
Message encodeLeave(PlayerId playerId);
bool decodeRequest(const Message& message, Request& request, std::string& error);

Message encodeWelcome(PlayerId playerId, const WorldSnapshot& snapshot);
Message encodeSnapshot(const WorldSnapshot& snapshot);
Message encodeError(const std::string& error);
Message encodeGoodbye();
bool decodeReply(const Message& message, Reply& reply, std::string& error);

} // namespace Network
