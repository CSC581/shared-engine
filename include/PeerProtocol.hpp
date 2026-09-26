#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Wire format for peer-to-peer play (Project 2 Section 5).
//
// This is a separate protocol from NetworkProtocol.hpp on purpose. The
// client-server protocol is a request/reply conversation with an authority:
// every message is addressed to the server and every reply is the server's
// view of the world. Peer messages are announcements — a peer states something
// about itself to everybody at once and nobody replies — so the shapes do not
// line up and folding them into one protocol would mean a message set where
// half the fields are meaningless in either direction.
//
// As in the client-server module a message is a vector of text fields, so the
// wire never depends on C++ struct layout, padding or byte order.
namespace Peer {

constexpr int protocolVersion = 4;

using PeerId = std::uint32_t;
using Message = std::vector<std::string>;

// How to reach one peer. Every peer publishes state on a PUB socket and
// answers introductions on a REP socket, so knowing a peer means knowing both.
struct PeerAddress {
    PeerId id = 0;
    std::string name;
    std::string pubEndpoint;
    std::string greetEndpoint;
};

// What one peer says about its own player. Generic network fields only — id,
// name, sequence, and pose. Anything a particular game invents (velocity,
// health, score, cargo, ammo) stays in the opaque data field so the peer
// module does not grow a dependency on one game's rules.
struct PeerState {
    PeerId id = 0;
    std::string name;
    // Per-sender counter. Arrival order is not send order, so a receiver drops
    // any state older than the newest one it already holds for that peer.
    std::uint64_t sequence = 0;
    float x = 0.0F;
    float y = 0.0F;
    // Whatever this particular game needs to say about a player beyond the
    // fields above. One opaque field, relayed without any peer looking inside.
    std::string data;
};

enum class MessageType {
    Hello,    // "here is how to reach me" — sent to a peer's greet socket
    Roster,   // reply to Hello: every peer the answerer knows about
    State,    // broadcast: my player's pose
    Ping,     // broadcast: I am still here, whatever my game is doing
    Leave,    // broadcast: I am going away
    Error,    // reply to a malformed Hello
};

// One decoded message. Only the member matching `type` is meaningful.
struct Envelope {
    MessageType type = MessageType::Error;
    PeerId senderId = 0;
    PeerAddress address;                // Hello
    std::vector<PeerAddress> roster;    // Roster
    PeerState state;                    // State
    std::string error;                  // Error
};

Message encodeHello(const PeerAddress& self);
Message encodeRoster(PeerId senderId, const std::vector<PeerAddress>& roster);
Message encodeState(const PeerState& state);

// Liveness on its own, with no game state attached.
//
// Peers decide a peer is gone by hearing nothing from it, so something has to
// keep arriving. Tying that to the game's own messages was the obvious choice
// and the wrong one: it makes a paused game indistinguishable from a crashed
// one, and it makes every game responsible for a networking rule it has no
// reason to know about. A session sends these on its own timer instead, so
// what a game publishes and how often is entirely the game's business.
Message encodePing(PeerId senderId);

Message encodeLeave(PeerId senderId);
Message encodeError(const std::string& error);

// Decodes any peer message. Returns false and fills `error` for a malformed
// message, an unknown command, or a protocol version this build cannot speak.
bool decode(const Message& message, Envelope& envelope, std::string& error);

} // namespace Peer
