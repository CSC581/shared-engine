#pragma once

#include "TimeSource.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Multiplayer for a game that does not want to care how multiplayer works.
//
// The engine now has two complete networking architectures under it — a
// client-server one (Network) and a peer-to-peer one (Peer) — and they have
// almost nothing in common at the wire level. One is a request/reply
// conversation with an authority that assigns identities and stores poses; the
// other is a mesh of equals announcing themselves to each other, with
// introductions, rosters and departures. A game written against either
// directly is written against that whole architecture, and moving it to the
// other one is a rewrite.
//
// But what a game actually wants from a network is much smaller than either
// design:
//
//     "here is where my player is" … "where is everybody else?"
//     "where are the shared world objects?" … "are we connected?"
//
// All four of those are answerable by both architectures, so they are what
// this interface exposes. A game picks its architecture by filling in a Config
// — one enum — and the rest of its code reads the same either way. That is the
// point: choosing between client-server and peer-to-peer becomes a deployment
// decision rather than a rewrite, and a game can offer both.
//
// What this deliberately does NOT cover is a design where the session owns the
// simulation rather than the game — a peer-to-peer scheme that agrees on
// inputs and has every machine compute the world itself, say. This interface
// is built the other way round: the game owns its simulation and the session
// moves data between copies of it. A scheme that inverts that does not belong
// behind the same interface, because the interface would then be lying about
// who computes what.
//
// SDL-free, like the modules underneath it.
namespace Multiplayer {

using PlayerId = std::uint32_t;

enum class Mode {
    // One server coordinates everyone. Simple, one place to look when
    // something is wrong, and one process whose death ends the session.
    ClientServer,
    // Players send their own data straight to each other. No relay in the
    // middle, so no single hop everything waits on — at the cost of every peer
    // needing to reach every other peer. Shared world objects still come from
    // an authority (see Config::serverEndpoint), which is the hybrid design.
    PeerToPeer,
};

enum class State {
    // Not usable yet, and still trying. Both modes retry by themselves.
    Connecting,
    // Usable: this player has an id and the session is exchanging data.
    Ready,
    // Not usable and not retrying. status() says why.
    Failed,
};

// Another player, as last heard from. Which hop that came over is exactly what
// this interface exists not to say.
struct Player {
    PlayerId id = 0;
    std::string name;
    float x = 0.0F;
    float y = 0.0F;
    // Whatever this game says about a player beyond where it is: health,
    // score, facing, animation state, whose turn it is.
    //
    // One opaque field, and deliberately so. The engine defines no structure
    // for it, offers no schema and no helpers, because every one of those
    // would be the engine taking a position on what a game's player is. The
    // game writes whatever it likes here and reads it back on the other side;
    // nothing between the two ends looks inside. A game adds, removes or
    // changes a field entirely within its own code.
    //
    // Empty for a game that has nothing to add, which costs one empty field
    // on the wire.
    std::string data;
};

// A world object owned by whoever holds authority, rather than by any player.
struct Platform {
    std::uint32_t id = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct Config {
    Mode mode = Mode::ClientServer;

    // ClientServer: the server to join; it assigns this player's id.
    // PeerToPeer: the shared-world authority that owns the platforms. Leave it
    // empty for a session with no shared objects, where peers exchange nothing
    // but each other's players and no server exists at all.
    std::string serverEndpoint = "tcp://127.0.0.1:5555";

    // Shown to other players, and carried by both architectures. May be empty.
    std::string playerName;

    // --- PeerToPeer only; ignored in ClientServer, where the server assigns
    // identity and there is nothing to bind or discover. ---

    // This player's id. Peers are not handed identities by anybody, so they
    // have to agree not to collide; ids must be unique across the session.
    PlayerId peerId = 1;
    // First of the two consecutive ports this peer binds: one to be introduced
    // on, one to broadcast on.
    int basePort = 7100;
    // The address other machines should reach this peer at. The default is
    // right for peers on one machine and wrong for any other case, because an
    // address bound on 0.0.0.0 is not one anybody else can dial.
    std::string advertiseHost = "127.0.0.1";
    // Any one running peer's introduction address is enough; its roster
    // introduces the rest.
    std::vector<std::string> bootstrapPeers;
};

class Session {
public:
    // Opens a session and starts connecting. Never throws for a network
    // reason: a refused connection, an address already in use or a server that
    // is not running yet all arrive as State::Failed or State::Connecting with
    // an explanation in status(), because every one of them is something a
    // game wants to draw on screen rather than catch.
    //
    // `realTime` must outlive the session, and must be real time rather than
    // game time — a session running on a paused clock could never reconnect,
    // and could never report the unpause.
    static std::unique_ptr<Session> open(Config config, const TimeSource& realTime);

    virtual ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Pump the network: hand the session the frame in which to deliver what
    // has arrived. Call it once a frame, including while the game is paused,
    // so that what other players are doing keeps arriving while this one is
    // not doing anything.
    virtual void update() = 0;

    // Where this player is now, and anything else this game wants other
    // players to know about it.
    //
    // Call it as often as the game needs and no more. Staying in the session
    // is not this call's job: a session announces itself on its own timer, so
    // a game that is paused, loading, or simply not moving anybody does not
    // quietly fall out of the session for having nothing to say.
    //
    // `data` is this game's own business; see Player::data. Both architectures
    // carry up to Net::maxPlayerDataLength bytes of it and neither interprets
    // any of them. A message that exceeds that is rejected by the receiver, so
    // a game with more to say than that needs a channel of its own.
    virtual void publishLocalPlayer(float x, float y, const std::string& data = {}) = 0;

    // Announce departure. Called by the destructor; call it earlier to leave
    // without tearing the session down.
    virtual void leave() = 0;

    // This player's id, or 0 before the session is Ready.
    virtual PlayerId localPlayerId() const = 0;

    // Everyone except this player, as of the last update(). Never includes the
    // local player, so a game can draw these and its own character without
    // filtering and without drawing itself twice.
    virtual const std::vector<Player>& remotePlayers() const = 0;

    // Shared world objects, empty if this session has no authority for them.
    //
    // These are the last set the authority sent. If the authority becomes
    // unreachable they stop moving but do not disappear, because a level does
    // not cease to exist when a server blinks — and a game whose floor
    // vanished would drop every player through the world. status() says when
    // they are no longer being refreshed. Remote players, by contrast, do
    // disappear when their owner is unreachable: a player nobody is steering
    // any more is a ghost.
    virtual const std::vector<Platform>& platforms() const = 0;

    virtual State state() const = 0;

    // One line fit to show a player: what is happening, or what went wrong.
    virtual std::string status() const = 0;

    virtual Mode mode() const = 0;

protected:
    Session() = default;
};

// "client-server" / "peer-to-peer", for logs and on-screen text.
const char* modeName(Mode mode);

// Parses those same two spellings; returns false for anything else.
bool parseMode(const std::string& text, Mode& mode);

} // namespace Multiplayer
