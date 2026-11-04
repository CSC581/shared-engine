#include "Multiplayer.hpp"

#include "EventCodec.hpp"
#include "NetworkClient.hpp"
#include "PeerSession.hpp"
#include "SendPacer.hpp"
#include "WorldStateClient.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
// its latest pose and any new events there, and the network thread leaves
// what it last heard and the events that arrived.
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
          client_(realTime, config_.serverEndpoint, config_.clientServerDataFormat,
                  config_.clientServerSnapshotStrategy),
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
            // Kept until taken, so a frame without apply() loses nothing. A
            // game that never takes them keeps only the newest.
            remoteEvents_.insert(remoteEvents_.end(), std::make_move_iterator(incomingEvents_.begin()),
                                 std::make_move_iterator(incomingEvents_.end()));
            incomingEvents_.clear();
            keepNewestEvents(remoteEvents_);
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

    // Checked here, on the game thread, so a bad event throws to the game
    // instead of on the network thread. Unlike poses, events are never
    // replaced: every one is handed to the client. After leave() nothing
    // would ever send them, so they are dropped rather than kept.
    bool supportsEvents() const override
    {
        return config_.clientServerDataFormat == Net::DataFormat::TextMultipart;
    }

    void publishEvent(const Event& event) override
    {
        if (!supportsEvents()) {
            throw std::logic_error("events require the text format");
        }
        std::string error;
        if (!Network::validateEvent(event, error)) {
            throw std::invalid_argument("cannot publish event: " + error);
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!leaveRequested_) {
            outgoingEvents_.push_back(event);
        }
    }

    std::vector<Event> takeRemoteEvents() override
    {
        std::vector<Event> events;
        events.swap(remoteEvents_);
        return events;
    }

    std::uint64_t droppedEvents() const override { return clientDropped_ + sessionDropped_; }

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
    // On leave with events still waiting, how long they get to be accepted
    // before LEAVE is sent anyway.
    static constexpr int leaveEventPolls = 200;

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
            std::vector<Event> events;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (leaveRequested_) {
                    break;
                }
                pose.swap(outbox_);
                events.swap(outgoingEvents_);
            }

            client_.poll();
            // Queued before the pose is sent, so they ride on the same request.
            for (const Event& event : events) {
                client_.publishEvent(event);
            }
            if (pose) {
                lastPose_ = std::move(pose);
                poseUnsent_ = true;
            }
            sendLastPose();
            publishIfChanged();
            std::this_thread::sleep_for(networkInterval);
        }

        // Events and a pose published just before leaving still count: hand
        // them over and keep carrying the events until the server accepts
        // them. A client that is reconnecting cannot send, so they are lost.
        std::vector<Event> lastEvents;
        std::optional<Pose> lastPublished;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            lastEvents.swap(outgoingEvents_);
            lastPublished.swap(outbox_);
        }
        for (const Event& event : lastEvents) {
            client_.publishEvent(event);
        }
        if (lastPublished) {
            lastPose_ = std::move(lastPublished);
        }
        // Only events are worth delaying LEAVE for; the pose just carries them.
        poseUnsent_ = false;
        for (int i = 0; client_.state() == Network::ConnectionState::Connected; ++i) {
            const bool eventsWaiting = lastPose_ && client_.pendingEvents() > 0;
            if (i >= (eventsWaiting ? leaveEventPolls : leaveGracePolls) ||
                (!client_.awaitingReply() && !sendLastPose())) {
                break;
            }
            std::this_thread::sleep_for(networkInterval);
            client_.poll();
        }
        client_.leave();
        publishIfChanged();
    }

    // Sends the newest pose once the client is free: a pose the game handed
    // over while a request was in flight (REQ allows one at a time), or the
    // last pose again to carry waiting events. Events travel on POSITION
    // requests, so with send pacing on (or paused, between heartbeats) they
    // would otherwise wait for the next paced pose; the server already has
    // that pose. Returns whether a pose or events are still waiting to go.
    bool sendLastPose()
    {
        if (!lastPose_ || (!poseUnsent_ && client_.pendingEvents() == 0)) {
            return false;
        }
        if (!client_.awaitingReply()) {
            client_.submitPosition(lastPose_->x, lastPose_->y, lastPose_->data);
            poseUnsent_ = false;
        }
        return true;
    }

    void keepNewestEvents(std::vector<Event>& events)
    {
        constexpr std::size_t limit = Network::NetworkClient::maxPendingEvents;
        if (events.size() > limit) {
            sessionDropped_ += events.size() - limit;
            events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(limit));
        }
    }

    void publishIfChanged()
    {
        std::vector<Event> arrived = client_.takeEvents();
        clientDropped_ = client_.droppedEvents();
        if (!arrived.empty()) {
            const std::lock_guard<std::mutex> lock(mutex_);
            incomingEvents_.insert(incomingEvents_.end(), std::make_move_iterator(arrived.begin()),
                                   std::make_move_iterator(arrived.end()));
            // A game that stops calling update() keeps only the newest.
            keepNewestEvents(incomingEvents_);
        }

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
    // The newest pose from the game, and whether it still has to go out. It
    // is resent to carry waiting events.
    std::optional<Pose> lastPose_;
    bool poseUnsent_ = false;

    // The mailbox: shared by both threads, guarded by mutex_.
    std::mutex mutex_;
    ClientView inbox_;
    std::optional<Pose> outbox_;
    std::vector<Event> outgoingEvents_;
    std::vector<Event> incomingEvents_;
    bool leaveRequested_ = false;
    std::atomic<std::uint64_t> clientDropped_{0};
    std::atomic<std::uint64_t> sessionDropped_{0};

    // Game thread only.
    ClientView view_;
    std::vector<Player> remote_;
    std::vector<Platform> platforms_;
    std::vector<Event> remoteEvents_;
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
            } else if (envelope.type == Peer::MessageType::Hello) {
                // A peer introduces itself when it starts, so a HELLO from an
                // id already held is that peer restarted after a crash, its
                // sequence counting from 1 again. Drop only the old count: the
                // player stays, so its object is not rebuilt for one frame.
                const auto restarted = known_.find(envelope.senderId);
                if (restarted != known_.end()) {
                    restarted->second.sequence = 0;
                }
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

    bool supportsEvents() const override { return false; }

    void publishEvent(const Event&) override
    {
        throw std::logic_error("events require client-server mode");
    }

    std::vector<Event> takeRemoteEvents() override { return {}; }

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
        if (config.clientServerDataFormat != Net::DataFormat::TextMultipart) {
            throw std::invalid_argument("binary data format is available only in client-server mode");
        }
        if (config.clientServerSnapshotStrategy != Net::SnapshotStrategy::FullSnapshot) {
            throw std::invalid_argument("property deltas are available only in client-server mode");
        }
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
