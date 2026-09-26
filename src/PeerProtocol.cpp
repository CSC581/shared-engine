#include "PeerProtocol.hpp"

#include "WireFormat.hpp"

#include <algorithm>
#include <limits>

namespace Peer {
namespace {

// Field encoding is shared with the client-server protocol; see WireFormat.hpp.
using Net::formatFloat;
using Net::parseFloat;
using Net::parseUnsigned;

bool parsePeerId(const std::string& text, PeerId& value)
{
    std::uint64_t parsed = 0;
    if (!parseUnsigned(text, parsed) || parsed == 0 || parsed > std::numeric_limits<PeerId>::max()) {
        return false;
    }
    value = static_cast<PeerId>(parsed);
    return true;
}

// A peer name is shown on screen and never interpreted, but it still travels
// between machines, so the shared rule applies: short and printable, rather
// than whatever the sender felt like putting in everyone else's window.
using Net::isValidName;

// Endpoints are handed straight to zmq_connect, so refuse anything that is not
// a transport this module actually dials.
bool isValidEndpoint(const std::string& endpoint)
{
    if (endpoint.size() < 8 || endpoint.size() > 128) {
        return false;
    }
    return endpoint.compare(0, 6, "tcp://") == 0 || endpoint.compare(0, 6, "ipc://") == 0;
}

bool parseAddress(const Message& message, std::size_t index, PeerAddress& address)
{
    if (message.size() < index + 4 || !parsePeerId(message[index], address.id) ||
        !isValidName(message[index + 1]) || !isValidEndpoint(message[index + 2]) ||
        !isValidEndpoint(message[index + 3])) {
        return false;
    }

    address.name = message[index + 1];
    address.pubEndpoint = message[index + 2];
    address.greetEndpoint = message[index + 3];
    return true;
}

void appendAddress(Message& message, const PeerAddress& address)
{
    message.push_back(std::to_string(address.id));
    message.push_back(address.name);
    message.push_back(address.pubEndpoint);
    message.push_back(address.greetEndpoint);
}

bool parseHeader(const Message& message, std::string& error)
{
    std::uint64_t version = 0;
    if (message.size() < 3 || !parseUnsigned(message[0], version)) {
        error = "message must start with a protocol version, command and sender";
        return false;
    }
    if (version != protocolVersion) {
        error = "unsupported peer protocol version";
        return false;
    }
    return true;
}

} // namespace

Message encodeHello(const PeerAddress& self)
{
    Message message{std::to_string(protocolVersion), "HELLO", std::to_string(self.id)};
    appendAddress(message, self);
    return message;
}

Message encodeRoster(PeerId senderId, const std::vector<PeerAddress>& roster)
{
    Message message{std::to_string(protocolVersion), "ROSTER", std::to_string(senderId),
                    std::to_string(roster.size())};
    for (const PeerAddress& address : roster) {
        appendAddress(message, address);
    }
    return message;
}

Message encodeState(const PeerState& state)
{
    Message message{std::to_string(protocolVersion),
                    "STATE",
                    std::to_string(state.id),
                    state.name,
                    std::to_string(state.sequence),
                    formatFloat(state.x),
                    formatFloat(state.y),
                    state.data};
    return message;
}

Message encodePing(PeerId senderId)
{
    return {std::to_string(protocolVersion), "PING", std::to_string(senderId)};
}

Message encodeLeave(PeerId senderId)
{
    return {std::to_string(protocolVersion), "LEAVE", std::to_string(senderId)};
}

Message encodeError(const std::string& error)
{
    // Sender 0 is never a valid peer id, so an error can never be mistaken for
    // a message from a peer.
    return {std::to_string(protocolVersion), "ERROR", "0", error};
}

bool decode(const Message& message, Envelope& envelope, std::string& error)
{
    if (!parseHeader(message, error)) {
        return false;
    }

    const std::string& command = message[1];
    envelope = {};

    if (command == "ERROR") {
        if (message.size() != 4) {
            error = "invalid ERROR message";
            return false;
        }
        envelope.type = MessageType::Error;
        envelope.error = message[3];
        return true;
    }

    if (!parsePeerId(message[2], envelope.senderId)) {
        error = "invalid sender id";
        return false;
    }

    if (command == "HELLO") {
        if (message.size() != 7 || !parseAddress(message, 3, envelope.address) ||
            envelope.address.id != envelope.senderId) {
            error = "invalid HELLO message";
            return false;
        }
        envelope.type = MessageType::Hello;
        return true;
    }

    if (command == "ROSTER") {
        std::uint64_t count = 0;
        constexpr std::size_t addressFieldCount = 4;
        if (message.size() < 4 || !parseUnsigned(message[3], count) || count > 64 ||
            message.size() != 4 + static_cast<std::size_t>(count) * addressFieldCount) {
            error = "invalid ROSTER message";
            return false;
        }
        envelope.roster.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            PeerAddress address;
            if (!parseAddress(message, 4 + static_cast<std::size_t>(i) * addressFieldCount, address)) {
                error = "invalid peer in ROSTER message";
                return false;
            }
            envelope.roster.push_back(std::move(address));
        }
        envelope.type = MessageType::Roster;
        return true;
    }

    if (command == "STATE") {
        if (message.size() != 8 || !isValidName(message[3]) ||
            !parseUnsigned(message[4], envelope.state.sequence) || envelope.state.sequence == 0 ||
            !parseFloat(message[5], envelope.state.x) || !parseFloat(message[6], envelope.state.y) ||
            !Net::isValidPlayerData(message[7])) {
            error = "invalid STATE message";
            return false;
        }
        envelope.type = MessageType::State;
        envelope.state.id = envelope.senderId;
        envelope.state.name = message[3];
        envelope.state.data = message[7];
        return true;
    }

    if (command == "PING") {
        if (message.size() != 3) {
            error = "invalid PING message";
            return false;
        }
        envelope.type = MessageType::Ping;
        return true;
    }

    if (command == "LEAVE") {
        if (message.size() != 3) {
            error = "invalid LEAVE message";
            return false;
        }
        envelope.type = MessageType::Leave;
        return true;
    }

    error = "unknown peer command";
    return false;
}

} // namespace Peer
