#include "Multiplayer.hpp"

#include "NetworkClient.hpp"
#include "PeerSession.hpp"
#include "WorldStateClient.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace Multiplayer {
namespace {

// Both architectures translate server-authored platforms the same way, and
// both keep the last set they were given while the authority link is down.
//
// The client drops its snapshot on a disconnect, which is right for players: a
// player whose owner is unreachable is a ghost, and drawing it is a lie. It is
// wrong for world objects. A platform that has stopped moving is stale but
// still the level geometry; a platform that has vanished is a hole in the
// floor, and every player standing on one falls through a world that was fine
// a moment ago. So the distinction is drawn here, at the layer that knows
// which is which, rather than in the client, which does not.
void refreshPlatforms(const std::vector<Network::PlatformState>& source, std::vector<Platform>& out)
{
    out.clear();
    for (const Network::PlatformState& platform : source) {
        out.push_back({platform.id, platform.x, platform.y, platform.width, platform.height});
    }
}

AuthorityState authorityStateOf(Network::ConnectionState state)
{
    switch (state) {
    case Network::ConnectionState::Connected:
        return AuthorityState::Ready;
    case Network::ConnectionState::Error:
        return AuthorityState::Failed;
    case Network::ConnectionState::Connecting:
    case Network::ConnectionState::Disconnected:
        return AuthorityState::Connecting;
    }
    return AuthorityState::Connecting;
}

// ---------------------------------------------------------------------------
// Client-server: join a server, tell it where this player is, and read back
// the world it keeps. Identity comes from the server.
// ---------------------------------------------------------------------------
class ClientServerSession final : public Session {
public:
    ClientServerSession(Config config, const TimeSource& realTime)
        : config_(std::move(config)), client_(realTime, config_.serverEndpoint)
    {
        // Sent with JOIN, so it must be set before starting.
        client_.setPlayerName(config_.playerName);
        client_.start();
    }

    ~ClientServerSession() override { client_.leave(); }

    void update() override
    {
        client_.poll();

        // Rebuild the view once per update rather than per query, so a game
        // that reads remotePlayers() three times in a frame pays for it once
        // and sees the same answer all three times.
        remote_.clear();
        for (const Network::PlayerState& player : client_.snapshot().players) {
            if (player.id != client_.playerId()) {
                remote_.push_back({player.id, player.name, player.x, player.y, player.data});
            }
        }

        if (client_.state() == Network::ConnectionState::Connected) {
            refreshPlatforms(client_.snapshot().platforms, platforms_);
        }
    }

    void publishLocalPlayer(float x, float y, const std::string& data) override
    {
        if (client_.state() == Network::ConnectionState::Connected) {
            client_.submitPosition(x, y, data);
        }
    }

    void leave() override { client_.leave(); }

    PlayerId localPlayerId() const override { return client_.playerId(); }
    const std::vector<Player>& remotePlayers() const override { return remote_; }
    const std::vector<Platform>& platforms() const override { return platforms_; }

    State state() const override
    {
        switch (client_.state()) {
        case Network::ConnectionState::Connected:
            return State::Ready;
        case Network::ConnectionState::Error:
            return State::Failed;
        case Network::ConnectionState::Connecting:
        case Network::ConnectionState::Disconnected:
            break;
        }
        return State::Connecting;
    }

    AuthorityState authorityState() const override { return authorityStateOf(client_.state()); }

    std::string status() const override
    {
        if (!client_.error().empty()) {
            return client_.error();
        }
        switch (client_.state()) {
        case Network::ConnectionState::Connected:
            return "connected to " + config_.serverEndpoint;
        case Network::ConnectionState::Disconnected:
            return "disconnected";
        default:
            break;
        }
        return "connecting to " + config_.serverEndpoint;
    }

    Mode mode() const override { return Mode::ClientServer; }

private:
    Config config_;
    Network::NetworkClient client_;
    std::vector<Player> remote_;
    std::vector<Platform> platforms_;
};

// ---------------------------------------------------------------------------
// Peer-to-peer: publish this player straight to the other peers and listen for
// theirs. Identity is chosen rather than assigned, because there is nobody to
// assign it.
//
// Shared world objects, if configured, come from a read-only authority client.
// Player poses are exchanged only through the peer mesh.
// ---------------------------------------------------------------------------
class PeerToPeerSession final : public Session {
public:
    PeerToPeerSession(Config config, const TimeSource& realTime) : config_(std::move(config))
    {
        Peer::PeerSession::Config mesh;
        mesh.id = config_.peerId;
        mesh.name = config_.playerName.empty() ? "player" : config_.playerName;
        mesh.advertiseHost = config_.advertiseHost;
        // Two consecutive ports: introductions on the first, broadcast on the
        // second. Bound on all interfaces; advertiseHost is what peers dial.
        mesh.greetBind = "tcp://0.0.0.0:" + std::to_string(config_.basePort);
        mesh.pubBind = "tcp://0.0.0.0:" + std::to_string(config_.basePort + 1);
        mesh.bootstrap = config_.bootstrapPeers;

        try {
            mesh_ = std::make_unique<Peer::PeerSession>(std::move(mesh));
            mesh_->start();
        } catch (const std::exception& exception) {
            // Almost always "that port is taken" — a second peer launched with
            // the same basePort. A game shows this; it should not have to
            // catch it.
            failure_ = std::string("could not start peer: ") + exception.what();
            mesh_.reset();
            return;
        }

        if (!config_.serverEndpoint.empty()) {
            authority_ = std::make_unique<Network::WorldStateClient>(realTime, config_.serverEndpoint);
            authority_->start();
        }
    }

    ~PeerToPeerSession() override { leave(); }

    void update() override
    {
        if (!mesh_) {
            return;
        }

        for (const Peer::Envelope& envelope : mesh_->drain()) {
            if (envelope.type == Peer::MessageType::State) {
                // Arrival order is not send order, so a stale pose must never
                // overwrite a newer one already held.
                Known& known = known_[envelope.state.id];
                if (envelope.state.sequence > known.sequence) {
                    known.sequence = envelope.state.sequence;
                    known.player = {envelope.state.id, envelope.state.name, envelope.state.x,
                                    envelope.state.y, envelope.state.data};
                }
            } else if (envelope.type == Peer::MessageType::Leave) {
                known_.erase(envelope.senderId);
            }
        }

        // Ordered by id, because a std::map is, so a game drawing them gets a
        // stable order rather than one that shuffles as peers come and go.
        remote_.clear();
        for (const auto& entry : known_) {
            if (entry.first != config_.peerId) {
                remote_.push_back(entry.second.player);
            }
        }

        if (authority_) {
            authority_->poll();
            if (authority_->state() == Network::ConnectionState::Connected) {
                refreshPlatforms(authority_->snapshot().platforms, platforms_);
            }
        }
    }

    void publishLocalPlayer(float x, float y, const std::string& data) override
    {
        if (!mesh_) {
            return;
        }

        Peer::PeerState state;
        state.id = config_.peerId;
        state.name = config_.playerName.empty() ? "player" : config_.playerName;
        state.sequence = ++sequence_;
        state.x = x;
        state.y = y;
        state.data = data;
        mesh_->publishState(state);
    }

    void leave() override
    {
        if (authority_) {
            authority_->stop();
        }
        // Destroying the mesh is what publishes LEAVE and stops its threads.
        mesh_.reset();
    }

    PlayerId localPlayerId() const override { return mesh_ ? config_.peerId : 0; }
    const std::vector<Player>& remotePlayers() const override { return remote_; }
    const std::vector<Platform>& platforms() const override { return platforms_; }

    State state() const override
    {
        if (!failure_.empty()) {
            return State::Failed;
        }
        // Ready as soon as this peer is publishing. A peer-to-peer session has
        // nobody to be accepted by, so being alone in it is a session with one
        // player in it, not a failure — and a game can already move, draw and
        // be joined.
        return mesh_ ? State::Ready : State::Connecting;
    }

    AuthorityState authorityState() const override
    {
        return authority_ ? authorityStateOf(authority_->state()) : AuthorityState::NotConfigured;
    }

    std::string status() const override
    {
        if (!failure_.empty()) {
            return failure_;
        }
        if (!mesh_) {
            return "starting peer";
        }
        const std::string transport = mesh_->lastError();
        if (!transport.empty()) {
            return transport;
        }

        std::string text = std::to_string(mesh_->peerCount()) + " peer(s) in mesh";
        if (authority_) {
            text += authority_->state() == Network::ConnectionState::Connected
                        ? ", world objects from " + config_.serverEndpoint
                        : ", waiting for world objects from " + config_.serverEndpoint;
        }
        return text;
    }

    Mode mode() const override { return Mode::PeerToPeer; }

private:
    struct Known {
        Player player;
        std::uint64_t sequence = 0;
    };

    Config config_;
    std::unique_ptr<Peer::PeerSession> mesh_;
    std::unique_ptr<Network::WorldStateClient> authority_;
    std::map<PlayerId, Known> known_;
    std::vector<Player> remote_;
    std::vector<Platform> platforms_;
    std::uint64_t sequence_ = 0;
    std::string failure_;
};

} // namespace

std::unique_ptr<Session> Session::open(Config config, const TimeSource& realTime)
{
    if (config.mode == Mode::PeerToPeer) {
        return std::make_unique<PeerToPeerSession>(std::move(config), realTime);
    }
    return std::make_unique<ClientServerSession>(std::move(config), realTime);
}

const char* modeName(Mode mode)
{
    return mode == Mode::PeerToPeer ? "peer-to-peer" : "client-server";
}

bool parseMode(const std::string& text, Mode& mode)
{
    if (text == "client-server") {
        mode = Mode::ClientServer;
        return true;
    }
    if (text == "peer-to-peer") {
        mode = Mode::PeerToPeer;
        return true;
    }
    return false;
}

} // namespace Multiplayer
