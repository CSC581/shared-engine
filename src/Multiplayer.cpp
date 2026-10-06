#include "Multiplayer.hpp"

#include "NetworkClient.hpp"
#include "PeerSession.hpp"
#include "SendPacer.hpp"
#include "WorldStateClient.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
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
//
// The NetworkClient lives on a network thread of its own, as the peer mesh's
// sockets do, so reading replies and sending poses never share the game loop.
// The two threads meet only in a small mailbox under mutex_: the game leaves
// its latest pose there, and the network thread leaves what it last heard.
// The client and its socket are created before that thread starts and
// destroyed after it is joined; starting and joining a thread are the memory
// barriers ZeroMQ asks for when a socket changes threads.
//
// `realTime` is read on the network thread, so it must be safe to read from
// another thread. RealTimeClock is.
// ---------------------------------------------------------------------------
class ClientServerSession final : public Session {
public:
    ClientServerSession(Config config, const TimeSource& realTime, const TimeSource* gameTime)
        : config_(std::move(config)),
          client_(realTime, config_.serverEndpoint),
          pacer_(gameTime, config_.sendIntervalGameTics, realTime, config_.heartbeatIntervalRealTics)
    {
        // Sent with JOIN, so it must be set before starting.
        client_.setPlayerName(config_.playerName);
        client_.start();

        // So the game sees "connecting" before the first update().
        copyClient(inbox_);
        view_ = inbox_;
        network_ = std::thread([this] { runNetwork(); });
    }

    ~ClientServerSession() override { leave(); }

    void update() override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (inbox_.version == view_.version) {
                return;
            }
            view_ = inbox_;
        }

        // Rebuild the view once per change rather than per query, so a game
        // that reads remotePlayers() three times in a frame pays for it once
        // and sees the same answer all three times.
        remote_.clear();
        for (const Network::PlayerState& player : view_.snapshot.players) {
            if (player.id != view_.playerId) {
                remote_.push_back({player.id, player.name, player.x, player.y, player.data});
            }
        }

        if (view_.state == Network::ConnectionState::Connected) {
            refreshPlatforms(view_.snapshot.platforms, platforms_);
        }
    }

    void publishLocalPlayer(float x, float y, const std::string& data) override
    {
        if (view_.state == Network::ConnectionState::Connected && pacer_.shouldSend()) {
            // Only the latest pose matters: an unsent older one is replaced,
            // so stale positions never queue up behind a slow reply.
            const std::lock_guard<std::mutex> lock(mutex_);
            outbox_ = Pose{x, y, data};
        }
    }

    void leave() override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            leaveRequested_ = true;
        }
        if (network_.joinable()) {
            network_.join();
        }
        update();
    }

    PlayerId localPlayerId() const override { return view_.playerId; }
    const std::vector<Player>& remotePlayers() const override { return remote_; }
    const std::vector<Platform>& platforms() const override { return platforms_; }

    State state() const override
    {
        switch (view_.state) {
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

    AuthorityState authorityState() const override { return authorityStateOf(view_.state); }

    std::string status() const override
    {
        if (!view_.error.empty()) {
            return view_.error;
        }
        switch (view_.state) {
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
    // What the network thread last heard, as the game reads it. `version`
    // changes whenever anything else does, so update() copies only then.
    struct ClientView {
        std::uint64_t version = 0;
        Network::ConnectionState state = Network::ConnectionState::Disconnected;
        Network::PlayerId playerId = 0;
        std::string error;
        Network::WorldSnapshot snapshot;
    };

    struct Pose {
        float x;
        float y;
        std::string data;
    };

    // How often the network thread wakes: the most a reply or a pose waits.
    static constexpr std::chrono::milliseconds networkInterval{1};
    // On leave, how long to wait for a reply already in flight. REQ cannot
    // send LEAVE until it has one, and without LEAVE the server only notices
    // the departure at its inactivity timeout.
    static constexpr int leaveGracePolls = 20;

    // Network thread only (and the constructor, before that thread exists).
    void copyClient(ClientView& view) const
    {
        view.state = client_.state();
        view.playerId = client_.playerId();
        view.error = client_.error();
        view.snapshot = client_.snapshot();
        ++view.version;
    }

    void runNetwork()
    {
        for (;;) {
            std::optional<Pose> pose;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (leaveRequested_) {
                    break;
                }
                pose.swap(outbox_);
            }

            client_.poll();
            if (pose) {
                client_.submitPosition(pose->x, pose->y, pose->data);
            }
            publishIfChanged();
            std::this_thread::sleep_for(networkInterval);
        }

        for (int i = 0; i < leaveGracePolls && client_.awaitingReply() &&
                        client_.state() == Network::ConnectionState::Connected;
             ++i) {
            std::this_thread::sleep_for(networkInterval);
            client_.poll();
        }
        client_.leave();
        publishIfChanged();
    }

    void publishIfChanged()
    {
        const Network::WorldSnapshot& snapshot = client_.snapshot();
        if (snapshot.serverTick == lastPublishedTick_ && client_.state() == lastPublishedState_ &&
            client_.playerId() == lastPublishedPlayerId_ && client_.error() == lastPublishedError_) {
            return;
        }
        lastPublishedTick_ = snapshot.serverTick;
        lastPublishedState_ = client_.state();
        lastPublishedPlayerId_ = client_.playerId();
        lastPublishedError_ = client_.error();

        const std::lock_guard<std::mutex> lock(mutex_);
        copyClient(inbox_);
    }

    Config config_;
    Network::NetworkClient client_;
    SendPacer pacer_;

    std::thread network_;
    // What publishIfChanged() last copied; network thread only.
    std::uint64_t lastPublishedTick_ = 0;
    Network::ConnectionState lastPublishedState_ = Network::ConnectionState::Disconnected;
    Network::PlayerId lastPublishedPlayerId_ = 0;
    std::string lastPublishedError_;

    // The mailbox: shared by both threads, guarded by mutex_.
    std::mutex mutex_;
    ClientView inbox_;
    std::optional<Pose> outbox_;
    bool leaveRequested_ = false;

    // Game thread only.
    ClientView view_;
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
    PeerToPeerSession(Config config, const TimeSource& realTime, const TimeSource* gameTime)
        : config_(std::move(config)),
          pacer_(gameTime, config_.sendIntervalGameTics, realTime, config_.heartbeatIntervalRealTics)
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
        if (!mesh_ || !pacer_.shouldSend()) {
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
            if (authority_->state() == Network::ConnectionState::Error) {
                text += ", world authority failed: " + authority_->error();
            } else if (authority_->state() == Network::ConnectionState::Connected) {
                text += ", world objects from " + config_.serverEndpoint;
            } else {
                text += ", waiting for world objects from " + config_.serverEndpoint;
            }
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
    SendPacer pacer_;
    std::unique_ptr<Peer::PeerSession> mesh_;
    std::unique_ptr<Network::WorldStateClient> authority_;
    std::map<PlayerId, Known> known_;
    std::vector<Player> remote_;
    std::vector<Platform> platforms_;
    std::uint64_t sequence_ = 0;
    std::string failure_;
};

} // namespace

std::unique_ptr<Session> Session::open(Config config, const TimeSource& realTime,
                                       const TimeSource* gameTime)
{
    if (config.mode == Mode::PeerToPeer) {
        return std::make_unique<PeerToPeerSession>(std::move(config), realTime, gameTime);
    }
    return std::make_unique<ClientServerSession>(std::move(config), realTime, gameTime);
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
