#include "PeerSession.hpp"

#include "Endpoint.hpp"
#include "ZmqMessage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Peer {
namespace {

// How long a blocking receive waits before looping, so a thread asked to stop
// notices within that long rather than at the next message.
constexpr int socketTimeoutMs = 200;

// How often the bootstrap thread retries peers it has not managed to introduce
// itself to yet, and how often it announces that this peer is still alive.
// Slow enough to be quiet, quick enough that starting peers in any order
// converges in about a second — and, against the timeout below, it takes ten
// missed announcements in a row before a peer is given up on.
constexpr auto bootstrapInterval = std::chrono::milliseconds(500);

// A peer that has said nothing for this long is presumed gone. Peers announce
// LEAVE on a clean exit, but a killed process, a pulled cable and a laptop lid
// closing all announce nothing at all, so silence has to mean something.
//
// What makes that safe is that silence is no longer the game's responsibility:
// this session announces itself on a timer whatever the game is doing, so a
// paused game, a game between levels and a game whose player is standing still
// all stay in the session. Only a peer that has genuinely stopped running
// stops being heard.
constexpr auto peerTimeout = std::chrono::seconds(5);

// An unread inbox means the game thread has stalled. Dropping the oldest
// messages bounds the memory instead of letting a stalled frame turn into an
// unbounded queue.
constexpr std::size_t maxInbox = 4096;

} // namespace

struct PeerSession::Impl {
    explicit Impl(Config configValue) : config(std::move(configValue))
    {
        self.id = config.id;
        self.name = config.name;
    }

    Config config;

    zmq::context_t context{1};

    // PUB is written by the game thread through publishState/Input/Checksum
    // and by the greeter thread on shutdown. A ZeroMQ socket may be used from
    // more than one thread as long as no two touch it at once and there is a
    // full memory barrier between uses, which is exactly what this mutex gives.
    std::mutex publishMutex;
    std::unique_ptr<zmq::socket_t> publisher;

    // The greet socket is bound in start() so its address can be advertised
    // straight away, then handed to the greeter thread. Creating that thread
    // is itself the full memory barrier ZeroMQ requires before a socket may be
    // used from a different thread.
    std::unique_ptr<zmq::socket_t> greeter;

    // Answering introductions and making them are separate threads on purpose.
    // Two peers greeting each other at the same moment would otherwise each be
    // blocked in a request while the other waited to be answered.
    std::thread subscriberThread;
    std::thread greeterThread;
    std::thread bootstrapThread;
    std::atomic<bool> running{false};

    mutable std::mutex stateMutex;
    PeerAddress self;
    // Everyone we know, ourselves included, keyed by id so a duplicate HELLO
    // is an update rather than a second copy of the same peer.
    std::map<PeerId, PeerAddress> known;
    // Peers we have subscribed to. The SUB socket lives on the subscriber
    // thread, so connects are queued here and picked up there.
    std::set<std::string> subscribed;
    std::vector<std::string> pendingSubscribes;
    // Greet endpoints we still owe a HELLO, and those already answered.
    std::set<std::string> pendingGreets;
    std::set<std::string> greeted;
    std::map<PeerId, std::chrono::steady_clock::time_point> lastHeard;
    std::deque<Envelope> inbox;
    std::string lastError;

    void setError(const std::string& message)
    {
        const std::lock_guard<std::mutex> lock(stateMutex);
        lastError = message;
    }

    // Callers must hold stateMutex. An endpoint change for the same id is a
    // duplicate launch, not an update: peers choose ids themselves, so this is
    // the first place the collision can be reported clearly.
    std::string peerIdConflictLocked(const PeerAddress& address) const
    {
        const auto differentEndpoint = [&address](const PeerAddress& existing) {
            return existing.pubEndpoint != address.pubEndpoint ||
                   existing.greetEndpoint != address.greetEndpoint;
        };

        if (address.id == self.id && differentEndpoint(self)) {
            return "peer id " + std::to_string(address.id) + " is already in use";
        }

        const auto existing = known.find(address.id);
        if (existing != known.end() && differentEndpoint(existing->second)) {
            return "peer id " + std::to_string(address.id) + " is already in use";
        }
        return {};
    }

    // Callers must hold stateMutex and must have checked peerIdConflictLocked.
    void rememberLocked(const PeerAddress& address)
    {
        if (address.id == 0 || address.id == self.id) {
            return;
        }

        known[address.id] = address;
        lastHeard[address.id] = std::chrono::steady_clock::now();

        if (subscribed.insert(address.pubEndpoint).second) {
            pendingSubscribes.push_back(address.pubEndpoint);
        }
        if (greeted.count(address.greetEndpoint) == 0) {
            pendingGreets.insert(address.greetEndpoint);
        }
    }

    void forget(PeerId id)
    {
        const std::lock_guard<std::mutex> lock(stateMutex);
        // The SUB stays connected. A peer that left may well come back on the
        // same address, and ZeroMQ reconnects a live SUB on its own; tearing
        // the connection down would mean missing the messages it sends on its
        // way back in.
        known.erase(id);
        lastHeard.erase(id);
    }

    void deliver(Envelope envelope)
    {
        const std::lock_guard<std::mutex> lock(stateMutex);
        if (inbox.size() >= maxInbox) {
            inbox.pop_front();
        }
        inbox.push_back(std::move(envelope));
    }

    void touch(PeerId id)
    {
        const std::lock_guard<std::mutex> lock(stateMutex);
        if (known.count(id) != 0) {
            lastHeard[id] = std::chrono::steady_clock::now();
        }
    }

    void expireSilentPeers()
    {
        std::vector<PeerId> expired;
        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const auto& entry : lastHeard) {
                if (now - entry.second > peerTimeout) {
                    expired.push_back(entry.first);
                }
            }
            for (const PeerId id : expired) {
                known.erase(id);
                lastHeard.erase(id);
            }
        }

        for (const PeerId id : expired) {
            // Report a timeout exactly as a LEAVE, so a game only ever has one
            // way of learning that a peer is gone.
            Envelope envelope;
            envelope.type = MessageType::Leave;
            envelope.senderId = id;
            deliver(std::move(envelope));
        }
    }

    bool publish(const Message& message)
    {
        const std::lock_guard<std::mutex> lock(publishMutex);
        if (!publisher) {
            return false;
        }
        return Net::send(*publisher, message, zmq::send_flags::dontwait);
    }

    // Answers HELLO from joining peers, and introduces us to peers we have
    // learned about but never greeted. Both halves live on one thread because
    // both touch the roster and neither is busy.
    // Answers HELLO from joining peers. Does nothing else, so a peer can
    // always be introduced to no matter what this peer is busy with.
    void runGreeter()
    {
        while (running.load()) {
            bool received = false;
            Message message;
            try {
                message = Net::receive(*greeter, received);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (!received) {
                continue;
            }

            Envelope envelope;
            std::string error;
            if (!decode(message, envelope, error) || envelope.type != MessageType::Hello) {
                Net::send(*greeter,
                          encodeError(error.empty() ? "greet socket accepts HELLO only" : error));
                continue;
            }

            std::vector<PeerAddress> reply;
            std::string conflict;
            {
                const std::lock_guard<std::mutex> lock(stateMutex);
                conflict = peerIdConflictLocked(envelope.address);
                if (conflict.empty()) {
                    rememberLocked(envelope.address);
                    // The newcomer gets everyone we know, so one introduction
                    // is enough to reach the whole mesh rather than just us.
                    reply.reserve(known.size() + 1);
                    reply.push_back(self);
                    for (const auto& entry : known) {
                        if (entry.first != envelope.address.id) {
                            reply.push_back(entry.second);
                        }
                    }
                    // They introduced themselves to us, so we owe them nothing.
                    greeted.insert(envelope.address.greetEndpoint);
                    pendingGreets.erase(envelope.address.greetEndpoint);
                }
            }

            if (!conflict.empty()) {
                // The joining peer receives this in status(), rather than
                // silently behaving as if it were the same player as us.
                Net::send(*greeter, encodeError(conflict));
                continue;
            }
            Net::send(*greeter, encodeRoster(self.id, reply));
            deliver(std::move(envelope));
        }
    }

    // Announces that this peer is alive, introduces it to everyone it has
    // heard of but never greeted, and drops peers that have gone quiet.
    void runBootstrap()
    {
        while (running.load()) {
            // Before anything that might take a moment: being late with this
            // is what gets a healthy peer thrown out of somebody's roster.
            publish(encodePing(self.id));
            introduceOurselves();
            expireSilentPeers();

            // Wake often enough to exit promptly when asked to stop.
            const auto wakeAt = std::chrono::steady_clock::now() + bootstrapInterval;
            while (running.load() && std::chrono::steady_clock::now() < wakeAt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
    }

    // Sends HELLO to every greet endpoint we know of and have not yet heard a
    // roster from. A short-lived REQ socket per attempt: a REQ that never gets
    // its reply is stuck forever, and a peer that is not up yet is the normal
    // case while a session is starting.
    void introduceOurselves()
    {
        std::vector<std::string> targets;
        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            targets.assign(pendingGreets.begin(), pendingGreets.end());
        }

        for (const std::string& target : targets) {
            if (!running.load()) {
                return;
            }
            if (target == self.greetEndpoint) {
                const std::lock_guard<std::mutex> lock(stateMutex);
                pendingGreets.erase(target);
                continue;
            }

            std::vector<PeerAddress> roster;
            try {
                zmq::socket_t requester(context, zmq::socket_type::req);
                requester.set(zmq::sockopt::linger, 0);
                requester.set(zmq::sockopt::rcvtimeo, socketTimeoutMs);
                requester.connect(target);
                if (!Net::send(requester, encodeHello(self))) {
                    continue;
                }

                bool received = false;
                const Message reply = Net::receive(requester, received);
                if (!received) {
                    continue;
                }

                Envelope envelope;
                std::string error;
                if (!decode(reply, envelope, error) || envelope.type != MessageType::Roster) {
                    setError(envelope.type == MessageType::Error
                                 ? "peer refused introduction: " + envelope.error
                                 : "unexpected reply to HELLO: " + error);
                    continue;
                }
                roster = std::move(envelope.roster);
            } catch (const zmq::error_t& exception) {
                setError(std::string("could not greet ") + target + ": " + exception.what());
                continue;
            }

            const std::lock_guard<std::mutex> lock(stateMutex);
            greeted.insert(target);
            pendingGreets.erase(target);
            std::string conflict;
            for (const PeerAddress& address : roster) {
                const std::string addressConflict = peerIdConflictLocked(address);
                if (addressConflict.empty()) {
                    rememberLocked(address);
                } else if (conflict.empty()) {
                    conflict = addressConflict;
                }
            }
            lastError = conflict;
        }
    }

    void runSubscriber()
    {
        std::unique_ptr<zmq::socket_t> subscriber;
        try {
            subscriber = std::make_unique<zmq::socket_t>(context, zmq::socket_type::sub);
            subscriber->set(zmq::sockopt::linger, 0);
            subscriber->set(zmq::sockopt::rcvtimeo, socketTimeoutMs);
            // Empty prefix: take every message. Peer messages are already
            // addressed to everybody, so there is nothing to filter on.
            subscriber->set(zmq::sockopt::subscribe, "");
        } catch (const zmq::error_t& exception) {
            setError(std::string("subscribe socket failed: ") + exception.what());
            return;
        }

        while (running.load()) {
            std::vector<std::string> connects;
            {
                const std::lock_guard<std::mutex> lock(stateMutex);
                connects.swap(pendingSubscribes);
            }
            for (const std::string& endpoint : connects) {
                try {
                    subscriber->connect(endpoint);
                } catch (const zmq::error_t& exception) {
                    setError(std::string("could not subscribe to ") + endpoint + ": " + exception.what());
                }
            }

            bool received = false;
            Message message;
            try {
                message = Net::receive(*subscriber, received);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (!received) {
                continue;
            }

            Envelope envelope;
            std::string error;
            if (!decode(message, envelope, error)) {
                setError("dropped malformed peer message: " + error);
                continue;
            }
            // Our own PUB is never subscribed to, but a peer could echo an id
            // we also use; ignoring it keeps one bad launch from corrupting
            // this peer's view of itself.
            if (envelope.senderId == self.id) {
                continue;
            }

            if (envelope.type == MessageType::Leave) {
                forget(envelope.senderId);
            } else {
                touch(envelope.senderId);
            }

            // A ping is liveness and nothing else. It has done its whole job
            // by the line above, so it stops here rather than filling a
            // game's inbox with messages it would only have to ignore.
            if (envelope.type == MessageType::Ping) {
                continue;
            }

            deliver(std::move(envelope));
        }
    }
};

PeerSession::PeerSession(Config config) : impl_(std::make_unique<Impl>(std::move(config)))
{
}

PeerSession::~PeerSession()
{
    if (impl_->running.load()) {
        impl_->publish(encodeLeave(impl_->self.id));
        // PUB drops a message it has no time to write. A brief pause gives the
        // socket a chance to flush LEAVE before the context is torn down, so
        // peers learn about a clean exit immediately instead of timing us out.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    impl_->running.store(false);
    // Join every thread before any socket or the context goes away: each one
    // is still inside a receive with a timeout, and tearing the context down
    // underneath them would be a use-after-free rather than a shutdown.
    if (impl_->subscriberThread.joinable()) {
        impl_->subscriberThread.join();
    }
    if (impl_->greeterThread.joinable()) {
        impl_->greeterThread.join();
    }
    if (impl_->bootstrapThread.joinable()) {
        impl_->bootstrapThread.join();
    }
    impl_->greeter.reset();
    {
        const std::lock_guard<std::mutex> lock(impl_->publishMutex);
        impl_->publisher.reset();
    }
}

void PeerSession::start()
{
    if (impl_->running.load()) {
        return;
    }

    std::string boundPub;
    try {
        const std::lock_guard<std::mutex> lock(impl_->publishMutex);
        impl_->publisher = std::make_unique<zmq::socket_t>(impl_->context, zmq::socket_type::pub);
        impl_->publisher->set(zmq::sockopt::linger, 0);
        impl_->publisher->bind(impl_->config.pubBind);
        boundPub = impl_->publisher->get(zmq::sockopt::last_endpoint);
    } catch (const zmq::error_t& exception) {
        throw std::runtime_error(std::string("could not bind peer publish socket on ") +
                                 impl_->config.pubBind + ": " + exception.what());
    }

    // Bound here rather than on the greeter thread: its address has to be
    // known now so the rest of start() can advertise it, and binding it twice
    // to read the address would leave a window where another process could
    // take the port.
    std::string boundGreet;
    try {
        impl_->greeter = std::make_unique<zmq::socket_t>(impl_->context, zmq::socket_type::rep);
        impl_->greeter->set(zmq::sockopt::linger, 0);
        // Wake periodically so the greeter thread notices a request to stop.
        impl_->greeter->set(zmq::sockopt::rcvtimeo, socketTimeoutMs);
        impl_->greeter->bind(impl_->config.greetBind);
        boundGreet = impl_->greeter->get(zmq::sockopt::last_endpoint);
    } catch (const zmq::error_t& exception) {
        impl_->greeter.reset();
        throw std::runtime_error(std::string("could not bind peer greet socket on ") +
                                 impl_->config.greetBind + ": " + exception.what());
    }

    const std::string advertisedPub =
        Net::rewriteTcpEndpointHost(boundPub, impl_->config.advertiseHost);
    const std::string advertisedGreet =
        Net::rewriteTcpEndpointHost(boundGreet, impl_->config.advertiseHost);

    {
        const std::lock_guard<std::mutex> lock(impl_->stateMutex);
        // A non-tcp bind (ipc:// in a test) has no host to rewrite; use it as
        // bound, which is already the address a peer would dial.
        impl_->self.pubEndpoint = advertisedPub.empty() ? boundPub : advertisedPub;
        impl_->self.greetEndpoint = advertisedGreet.empty() ? boundGreet : advertisedGreet;
        for (const std::string& endpoint : impl_->config.bootstrap) {
            if (!endpoint.empty() && endpoint != impl_->self.greetEndpoint) {
                impl_->pendingGreets.insert(endpoint);
            }
        }
    }
    impl_->running.store(true);
    impl_->subscriberThread = std::thread([this] { impl_->runSubscriber(); });
    impl_->greeterThread = std::thread([this] { impl_->runGreeter(); });
    impl_->bootstrapThread = std::thread([this] { impl_->runBootstrap(); });
}

void PeerSession::publishState(const PeerState& state)
{
    if (!impl_->publish(encodeState(state))) {
        impl_->setError("could not publish peer state");
    }
}

std::vector<Envelope> PeerSession::drain()
{
    const std::lock_guard<std::mutex> lock(impl_->stateMutex);
    std::vector<Envelope> drained(std::make_move_iterator(impl_->inbox.begin()),
                                  std::make_move_iterator(impl_->inbox.end()));
    impl_->inbox.clear();
    return drained;
}

std::vector<PeerAddress> PeerSession::roster() const
{
    const std::lock_guard<std::mutex> lock(impl_->stateMutex);
    std::vector<PeerAddress> addresses;
    addresses.reserve(impl_->known.size() + 1);
    addresses.push_back(impl_->self);
    for (const auto& entry : impl_->known) {
        addresses.push_back(entry.second);
    }
    std::sort(addresses.begin(), addresses.end(),
              [](const PeerAddress& left, const PeerAddress& right) { return left.id < right.id; });
    return addresses;
}

bool PeerSession::knows(PeerId id) const
{
    const std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return id == impl_->self.id || impl_->known.count(id) != 0;
}

std::size_t PeerSession::peerCount() const
{
    const std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->known.size() + 1;
}

PeerAddress PeerSession::self() const
{
    const std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->self;
}

std::string PeerSession::lastError() const
{
    const std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->lastError;
}

} // namespace Peer
