#include "NetworkServerHost.hpp"

#include "BinaryNetworkProtocol.hpp"
#include "Endpoint.hpp"
#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <zmq.hpp>

#include <atomic>
#include <future>
#include <iomanip>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Network {
namespace {

struct DeliveryState {
    std::mutex mutex;
    Net::SnapshotStrategy strategy = Net::SnapshotStrategy::FullSnapshot;
    std::optional<WorldSnapshot> lastSentSnapshot;
};

// Shared between one worker thread and the handshake that started it.
struct WorkerState {
    // Cleared to ask the worker to exit (failed JOIN, shutdown).
    std::atomic<bool> alive{true};
    // Set by the worker as the very last thing it does, so the handshake can
    // join it without blocking. `alive` alone is not enough: a cancelled
    // worker can still be sitting in its receive timeout.
    std::atomic<bool> finished{false};
    std::shared_ptr<DeliveryState> delivery = std::make_shared<DeliveryState>();
};

struct Worker {
    std::shared_ptr<WorkerState> state;
    std::thread thread;
};

// Addresses a socket can bind to but no client can dial.
bool isWildcardHost(const std::string& host)
{
    return host == "*" || host == "0.0.0.0" || host == "::" || host == "[::]";
}

void validate(const HostConfig& config)
{
    if (config.bindEndpoint.empty()) {
        throw std::invalid_argument("NetworkServerHost bind endpoint is empty");
    }
    if (config.tickInterval.count() <= 0 || config.pollInterval.count() <= 0 ||
        config.workerStartTimeout.count() <= 0 || config.trafficLogInterval.count() < 0) {
        throw std::invalid_argument("NetworkServerHost intervals must be positive");
    }
    if (config.mode == HostMode::Dedicated && isWildcardHost(config.advertiseHost)) {
        throw std::invalid_argument("NetworkServerHost must advertise a host clients can dial, not " +
                                    config.advertiseHost);
    }
    if (config.mode == HostMode::Dedicated &&
        Net::rewriteTcpEndpointHost("tcp://0.0.0.0:1", config.advertiseHost).empty()) {
        throw std::invalid_argument("NetworkServerHost advertise host is not usable");
    }
}

Net::DataFormat detectFormat(const Message& message)
{
    if (!message.empty() && hasBinaryMagic(message.front())) {
        return Net::DataFormat::Binary;
    }
    return Net::DataFormat::TextMultipart;
}

bool decodeTransportRequest(const Message& message, Net::DataFormat format,
                            BinaryRequestEnvelope& envelope, std::string& error)
{
    if (format == Net::DataFormat::Binary) {
        if (message.size() != 1) {
            error = "binary request must contain exactly one frame";
            return false;
        }
        return decodeBinaryRequest(message.front(), envelope, error);
    }
    envelope = {};
    return decodeRequest(message, envelope.request, error);
}

Message encodeTransportReply(const BinaryReplyEnvelope& envelope, Net::DataFormat format)
{
    if (format == Net::DataFormat::Binary) {
        return {encodeBinaryReply(envelope)};
    }
    return encodeReply(envelope.reply);
}

Message encodeTransportReply(const Reply& reply, Net::DataFormat format)
{
    BinaryReplyEnvelope envelope;
    envelope.reply = reply;
    return encodeTransportReply(envelope, format);
}

Reply errorReply(std::string error)
{
    Reply reply;
    reply.type = ReplyType::Error;
    reply.error = std::move(error);
    return reply;
}

bool isAcceptedLeave(const Request& request, bool decoded, const Reply& reply)
{
    return decoded && request.type == RequestType::Leave && reply.type == ReplyType::Goodbye;
}

} // namespace

struct NetworkServerHost::Impl {
    Impl(ServerConfig serverConfig, HostConfig hostConfig)
        : config(std::move(hostConfig)),
          server(clock, std::move(serverConfig))
    {
        validate(config);
    }

    void log(const std::string& line) const
    {
        if (config.log) {
            config.log(line);
        }
    }

    void resetDelivery(const std::shared_ptr<DeliveryState>& delivery,
                       Net::SnapshotStrategy strategy, const WorldSnapshot& snapshot)
    {
        const std::lock_guard<std::mutex> lock(delivery->mutex);
        delivery->strategy = strategy;
        delivery->lastSentSnapshot = snapshot;
    }

    bool deliveryMatches(const std::shared_ptr<DeliveryState>& delivery,
                         const BinaryRequestEnvelope& request)
    {
        if (request.request.type != RequestType::Position) return true;
        const std::lock_guard<std::mutex> lock(delivery->mutex);
        return delivery->strategy == request.snapshotStrategy;
    }

    bool sendSessionReply(zmq::socket_t& socket, const Reply& serverReply,
                          Net::DataFormat format, const BinaryRequestEnvelope& request,
                          const std::shared_ptr<DeliveryState>& delivery)
    {
        // Keep selection, transmission and baseline advancement atomic with
        // respect to a rejoin resetting this session's delivery state.
        const std::lock_guard<std::mutex> lock(delivery->mutex);
        BinaryReplyEnvelope outgoing;
        outgoing.reply = serverReply;
        std::optional<WorldSnapshot> sentSnapshot;
        if (delivery->strategy == Net::SnapshotStrategy::PropertyDelta &&
            format == Net::DataFormat::Binary && request.request.type == RequestType::Position &&
            serverReply.type == ReplyType::Snapshot) {
            sentSnapshot = serverReply.snapshot;
            if (!request.forceFullSnapshot && delivery->lastSentSnapshot &&
                request.acknowledgedServerTick == delivery->lastSentSnapshot->serverTick) {
                outgoing.kind = BinaryReplyKind::SnapshotDelta;
                outgoing.delta = makeSnapshotDelta(*delivery->lastSentSnapshot,
                                                   serverReply.snapshot);
            }
        }

        const bool sent = Net::send(socket, encodeTransportReply(outgoing, format));
        if (sent && sentSnapshot) {
            delivery->lastSentSnapshot = std::move(*sentSnapshot);
        }
        return sent;
    }

    // Server-owned platforms move on real time whether or not anybody is
    // asking, so they get a thread of their own rather than being advanced
    // only when a request happens to arrive.
    void runTicks()
    {
        auto lastTrafficLog = std::chrono::steady_clock::now();
        std::unordered_map<PlayerId, std::uint64_t> lastCounts;
        while (running.load()) {
            server.update();
            {
                const std::lock_guard<std::mutex> lock(listenDeliveriesMutex);
                for (auto entry = listenDeliveries.begin(); entry != listenDeliveries.end();) {
                    if (!server.hasSession(entry->first)) {
                        entry = listenDeliveries.erase(entry);
                    } else {
                        ++entry;
                    }
                }
            }
            if (config.trafficLogInterval.count() > 0 && config.log) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastTrafficLog >= config.trafficLogInterval) {
                    logTraffic(std::chrono::duration<double>(now - lastTrafficLog).count(), lastCounts);
                    lastTrafficLog = now;
                }
            }
            std::this_thread::sleep_for(config.tickInterval);
        }
    }

    // One line per player: accepted POSITIONs per second since the last line.
    // A player who joined during the interval is measured from zero, so its
    // first reading is low.
    void logTraffic(double seconds, std::unordered_map<PlayerId, std::uint64_t>& lastCounts) const
    {
        std::unordered_map<PlayerId, std::uint64_t> counts;
        for (const PlayerTraffic& player : server.traffic()) {
            const auto previous = lastCounts.find(player.id);
            const std::uint64_t before = previous == lastCounts.end() ? 0 : previous->second;
            std::ostringstream line;
            line.imbue(std::locale::classic());
            line << "player " << player.id;
            if (!player.name.empty()) {
                line << " (" << player.name << ")";
            }
            line << ": " << std::fixed << std::setprecision(1)
                 << static_cast<double>(player.acceptedPositions - before) / seconds
                 << " accepted POSITION/s";
            log(line.str());
            counts[player.id] = player.acceptedPositions;
        }
        lastCounts = std::move(counts);
    }

    // HostMode::Listen: every request answered on the one socket.
    void runListen(zmq::socket_t socket)
    {
        while (running.load()) {
            bool received = false;
            Message request;
            try {
                request = Net::receive(socket, received);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (received) {
                const Net::DataFormat format = detectFormat(request);
                BinaryRequestEnvelope decoded;
                std::string error;
                const bool valid = decodeTransportRequest(request, format, decoded, error);
                if (!valid) {
                    Net::send(socket, encodeTransportReply(errorReply(std::move(error)), format));
                    continue;
                }
                Reply reply;
                std::shared_ptr<DeliveryState> delivery;
                if (decoded.request.type == RequestType::Join) {
                    reply = server.handle(decoded.request);
                    if (reply.type == ReplyType::Welcome) {
                        const std::lock_guard<std::mutex> lock(listenDeliveriesMutex);
                        delivery = listenDeliveries[decoded.request.sessionToken];
                        if (!delivery) {
                            delivery = listenDeliveries[decoded.request.sessionToken] =
                                std::make_shared<DeliveryState>();
                        }
                    }
                } else {
                    {
                        const std::lock_guard<std::mutex> lock(listenDeliveriesMutex);
                        const auto found = listenDeliveries.find(decoded.request.sessionToken);
                        if (found != listenDeliveries.end()) delivery = found->second;
                    }
                    if (delivery && !deliveryMatches(delivery, decoded)) {
                        reply = errorReply("snapshot strategy does not match this session");
                    } else {
                        reply = server.handle(decoded.request);
                    }
                }
                if (decoded.request.type == RequestType::Join && reply.type == ReplyType::Welcome && delivery) {
                    resetDelivery(delivery, decoded.snapshotStrategy, reply.snapshot);
                    BinaryReplyEnvelope outgoing;
                    outgoing.reply = reply;
                    if (decoded.snapshotStrategy == Net::SnapshotStrategy::PropertyDelta)
                        outgoing.kind = BinaryReplyKind::DeltaWelcome;
                    Net::send(socket, encodeTransportReply(outgoing, format));
                } else if (delivery) {
                    sendSessionReply(socket, reply, format, decoded, delivery);
                } else {
                    Net::send(socket, encodeTransportReply(reply, format));
                }
                if (decoded.request.type == RequestType::Leave && reply.type == ReplyType::Goodbye) {
                    const std::lock_guard<std::mutex> lock(listenDeliveriesMutex);
                    listenDeliveries.erase(decoded.request.sessionToken);
                }
            }
        }
    }

    // One blocking REP loop per connected client. A slow client only stalls
    // this thread; other workers keep serving. The socket is created and used
    // only on this thread, because ZeroMQ sockets are not thread-safe.
    void runWorker(std::promise<std::string> endpointReady, std::shared_ptr<WorkerState> state,
                   SessionToken sessionToken)
    {
        try {
            zmq::socket_t socket(context, zmq::socket_type::rep);
            socket.set(zmq::sockopt::linger, 0);
            socket.set(zmq::sockopt::rcvtimeo, static_cast<int>(config.pollInterval.count()));
            // All interfaces; WELCOME advertises config.advertiseHost instead.
            socket.bind("tcp://0.0.0.0:*");
            const std::string endpoint = socket.get(zmq::sockopt::last_endpoint);
            endpointReady.set_value(endpoint);

            // Set once this worker's player is seen to exist. The handshake
            // registers the player only after the worker has bound, so "no
            // session yet" at startup is normal and must not end the worker.
            bool registered = false;
            while (state->alive.load() && running.load()) {
                Message requestMessage;
                bool received = false;
                try {
                    requestMessage = Net::receive(socket, received);
                } catch (const zmq::error_t&) {
                    continue;
                }
                if (!received) {
                    if (sessionEnded(sessionToken, state, endpoint, registered)) {
                        break;
                    }
                    continue;
                }

                const Net::DataFormat format = detectFormat(requestMessage);
                BinaryRequestEnvelope request;
                std::string error;
                const bool decoded = decodeTransportRequest(requestMessage, format, request, error);
                Reply reply;
                if (!decoded) {
                    reply = errorReply(std::move(error));
                } else if (!deliveryMatches(state->delivery, request)) {
                    reply = errorReply("snapshot strategy does not match this session");
                } else {
                    reply = server.handle(request.request);
                }
                if (decoded) {
                    sendSessionReply(socket, reply, format, request, state->delivery);
                } else {
                    Net::send(socket, encodeTransportReply(reply, format));
                }

                // A malformed or unauthorized LEAVE produces ERROR. Keep this
                // worker alive unless the server actually accepted the departure.
                if (isAcceptedLeave(request.request, decoded, reply)) {
                    break;
                }
            }
        } catch (const std::exception& exception) {
            log(std::string("worker failed: ") + exception.what());
            try {
                endpointReady.set_value({});
            } catch (const std::future_error&) {
                // The endpoint was already published before the failure.
            }
        } catch (...) {
            log("worker failed with an unknown exception");
            try {
                endpointReady.set_value({});
            } catch (const std::future_error&) {
            }
        }

        {
            const std::lock_guard<std::mutex> lock(endpointsMutex);
            const auto entry = endpointsByToken.find(sessionToken);
            if (entry != endpointsByToken.end() && entry->second.owner == state) {
                endpointsByToken.erase(entry);
            }
        }
        state->alive.store(false);
        state->finished.store(true);
    }

    // A client that crashes never sends LEAVE; the server expires its player
    // after ServerConfig::inactivityTimeoutTics, and from then on this worker
    // has nobody to serve. Checked, and the endpoint entry dropped, under
    // endpointsMutex — the lock rejoin() holds while it re-registers a token —
    // so a rejoin either revives the session before this check (the worker
    // stays) or finds no entry after it (and gets a fresh worker). It can
    // never be handed the address of a worker that is about to exit.
    bool sessionEnded(const SessionToken& sessionToken, const std::shared_ptr<WorkerState>& state,
                      const std::string& endpoint, bool& registered)
    {
        const std::lock_guard<std::mutex> lock(endpointsMutex);
        if (server.hasSession(sessionToken)) {
            registered = true;
            return false;
        }
        if (!registered) {
            return false;
        }
        const auto entry = endpointsByToken.find(sessionToken);
        if (entry != endpointsByToken.end() && entry->second.owner == state) {
            endpointsByToken.erase(entry);
        }
        // The endpoint, not the token: the token is the client's credential.
        log("worker " + endpoint + " exiting: its player was expired without LEAVE");
        return true;
    }

    // Joins and drops workers that have exited, so a long-running server does
    // not accumulate one dead thread per client it has ever had.
    void reapFinishedWorkers()
    {
        const std::lock_guard<std::mutex> lock(workersMutex);
        for (auto worker = workers.begin(); worker != workers.end();) {
            if (worker->state->finished.load()) {
                if (worker->thread.joinable()) {
                    worker->thread.join();
                }
                worker = workers.erase(worker);
            } else {
                ++worker;
            }
        }
    }

    // Answers a JOIN whose session token already has a live worker by
    // pointing it back at that worker. Returns false if there is none.
    bool rejoin(zmq::socket_t& handshake, const BinaryRequestEnvelope& envelope,
                Net::DataFormat format)
    {
        const Request& request = envelope.request;
        const std::lock_guard<std::mutex> lock(endpointsMutex);
        const auto existing = endpointsByToken.find(request.sessionToken);
        if (existing == endpointsByToken.end()) {
            return false;
        }

        Reply welcome = server.handle(request);
        if (welcome.type != ReplyType::Welcome) {
            Net::send(handshake, encodeTransportReply(welcome, format));
            return true;
        }
        welcome.sessionEndpoint = existing->second.endpoint;
        resetDelivery(existing->second.owner->delivery, envelope.snapshotStrategy, welcome.snapshot);
        BinaryReplyEnvelope outgoing;
        outgoing.reply = welcome;
        if (envelope.snapshotStrategy == Net::SnapshotStrategy::PropertyDelta)
            outgoing.kind = BinaryReplyKind::DeltaWelcome;
        Net::send(handshake, encodeTransportReply(outgoing, format));
        return true;
    }

    void join(zmq::socket_t& handshake, const BinaryRequestEnvelope& envelope,
              Net::DataFormat format)
    {
        const Request& request = envelope.request;
        if (rejoin(handshake, envelope, format)) {
            return;
        }

        auto state = std::make_shared<WorkerState>();
        std::promise<std::string> endpointReady;
        std::future<std::string> endpointFuture = endpointReady.get_future();
        {
            const std::lock_guard<std::mutex> lock(workersMutex);
            workers.push_back({state, std::thread(&Impl::runWorker, this, std::move(endpointReady),
                                                  state, request.sessionToken)});
        }

        // Bounded, so one worker that never binds cannot hold up every other
        // client's JOIN on this handshake.
        std::string boundWorker;
        if (endpointFuture.wait_for(config.workerStartTimeout) == std::future_status::ready) {
            boundWorker = endpointFuture.get();
        }
        const std::string sessionEndpoint = Net::rewriteTcpEndpointHost(boundWorker, config.advertiseHost);
        if (sessionEndpoint.empty()) {
            state->alive.store(false);
            Net::send(handshake, encodeTransportReply(errorReply("could not start client worker"), format));
            return;
        }

        Reply welcome = server.handle(request);
        if (welcome.type != ReplyType::Welcome) {
            // Server full, for example. The worker was never announced.
            state->alive.store(false);
            Net::send(handshake, encodeTransportReply(welcome, format));
            return;
        }

        {
            const std::lock_guard<std::mutex> lock(endpointsMutex);
            endpointsByToken[request.sessionToken] = {sessionEndpoint, state};
        }
        log("player " + std::to_string(welcome.playerId) + " -> " + sessionEndpoint);
        welcome.sessionEndpoint = sessionEndpoint;
        resetDelivery(state->delivery, envelope.snapshotStrategy, welcome.snapshot);
        BinaryReplyEnvelope outgoing;
        outgoing.reply = welcome;
        if (envelope.snapshotStrategy == Net::SnapshotStrategy::PropertyDelta)
            outgoing.kind = BinaryReplyKind::DeltaWelcome;
        Net::send(handshake, encodeTransportReply(outgoing, format));
    }

    // HostMode::Dedicated: the public socket accepts JOIN (which hands the
    // client its own worker) and GET_WORLD (a read-only view for hybrid peers,
    // which never join the roster). Nothing else is served here.
    void runHandshake(zmq::socket_t handshake)
    {
        while (running.load()) {
            reapFinishedWorkers();

            Message requestMessage;
            bool received = false;
            try {
                requestMessage = Net::receive(handshake, received);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (!received) {
                continue;
            }

            BinaryRequestEnvelope request;
            std::string error;
            const Net::DataFormat format = detectFormat(requestMessage);
            if (!decodeTransportRequest(requestMessage, format, request, error)) {
                Net::send(handshake, encodeTransportReply(errorReply(std::move(error)), format));
            } else if (request.request.type == RequestType::GetWorld) {
                Net::send(handshake, encodeTransportReply(server.handle(request.request), format));
            } else if (request.request.type != RequestType::Join) {
                Net::send(handshake, encodeTransportReply(errorReply("handshake accepts JOIN only"), format));
            } else {
                join(handshake, request, format);
            }
        }
    }

    struct Session {
        std::string endpoint;
        // The worker serving this token, so a worker that exits late cannot
        // erase an entry that a newer worker for the same token now owns.
        std::shared_ptr<WorkerState> owner;
    };

    HostConfig config;
    RealTimeClock clock;
    NetworkServer server;
    zmq::context_t context{1};

    std::atomic<bool> running{false};
    bool started = false;
    bool stopped = false;
    // Serializes start() and stop() against each other.
    std::mutex lifecycleMutex;
    mutable std::mutex boundMutex;
    std::string bound;

    std::thread ticks;
    std::thread serve;

    mutable std::mutex workersMutex;
    std::vector<Worker> workers;

    std::mutex endpointsMutex;
    std::unordered_map<SessionToken, Session> endpointsByToken;
    std::mutex listenDeliveriesMutex;
    std::unordered_map<SessionToken, std::shared_ptr<DeliveryState>> listenDeliveries;
};

NetworkServerHost::NetworkServerHost(ServerConfig serverConfig, HostConfig hostConfig)
    : impl_(std::make_unique<Impl>(std::move(serverConfig), std::move(hostConfig)))
{
}

NetworkServerHost::~NetworkServerHost()
{
    stop();
}

void NetworkServerHost::start()
{
    const std::lock_guard<std::mutex> lifecycle(impl_->lifecycleMutex);
    if (impl_->started) {
        throw std::logic_error("NetworkServerHost can only be started once");
    }

    zmq::socket_t socket;
    std::string bound;
    try {
        socket = zmq::socket_t(impl_->context, zmq::socket_type::rep);
        socket.set(zmq::sockopt::linger, 0);
        socket.set(zmq::sockopt::rcvtimeo, static_cast<int>(impl_->config.pollInterval.count()));
        socket.bind(impl_->config.bindEndpoint);
        bound = socket.get(zmq::sockopt::last_endpoint);
    } catch (const zmq::error_t& error) {
        throw std::runtime_error("could not bind " + impl_->config.bindEndpoint + ": " + error.what());
    }

    {
        const std::lock_guard<std::mutex> lock(impl_->boundMutex);
        impl_->bound = bound;
    }
    impl_->started = true;
    impl_->running.store(true);
    impl_->ticks = std::thread(&Impl::runTicks, impl_.get());
    // The socket moves onto the thread that owns it from here on; creating the
    // thread is the memory barrier ZeroMQ asks for when a socket changes hands.
    if (impl_->config.mode == HostMode::Dedicated) {
        impl_->serve = std::thread([impl = impl_.get(), moved = std::move(socket)]() mutable {
            impl->runHandshake(std::move(moved));
        });
    } else {
        impl_->serve = std::thread([impl = impl_.get(), moved = std::move(socket)]() mutable {
            impl->runListen(std::move(moved));
        });
    }
    impl_->log("listening on " + bound);
}

void NetworkServerHost::stop()
{
    const std::lock_guard<std::mutex> lifecycle(impl_->lifecycleMutex);
    if (!impl_->started || impl_->stopped) {
        return;
    }
    impl_->stopped = true;

    // Stop background work while the server and context are still alive, so
    // no thread outlives an object it uses.
    impl_->running.store(false);
    {
        const std::lock_guard<std::mutex> lock(impl_->workersMutex);
        for (Worker& worker : impl_->workers) {
            worker.state->alive.store(false);
        }
    }
    try {
        // Wakes every blocked receive with ETERM.
        impl_->context.shutdown();
    } catch (const zmq::error_t&) {
        // Already shutting down.
    }

    if (impl_->ticks.joinable()) {
        impl_->ticks.join();
    }
    // The handshake can still be starting a worker; join it before the workers
    // so that one is joined too.
    if (impl_->serve.joinable()) {
        impl_->serve.join();
    }
    const std::lock_guard<std::mutex> lock(impl_->workersMutex);
    for (Worker& worker : impl_->workers) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
    impl_->workers.clear();
}

bool NetworkServerHost::running() const
{
    return impl_->running.load();
}

std::string NetworkServerHost::boundEndpoint() const
{
    const std::lock_guard<std::mutex> lock(impl_->boundMutex);
    return impl_->bound;
}

std::size_t NetworkServerHost::playerCount() const
{
    return impl_->server.playerCount();
}

std::vector<PlayerTraffic> NetworkServerHost::traffic() const
{
    return impl_->server.traffic();
}

std::size_t NetworkServerHost::activeWorkers() const
{
    const std::lock_guard<std::mutex> lock(impl_->workersMutex);
    return impl_->workers.size();
}

} // namespace Network
