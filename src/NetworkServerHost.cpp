#include "NetworkServerHost.hpp"

#include "Endpoint.hpp"
#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <zmq.hpp>

#include <atomic>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Network {
namespace {

// Shared between one worker thread and the handshake that started it.
struct WorkerState {
    // Cleared to ask the worker to exit (failed JOIN, shutdown).
    std::atomic<bool> alive{true};
    // Set by the worker as the very last thing it does, so the handshake can
    // join it without blocking. `alive` alone is not enough: a cancelled
    // worker can still be sitting in its receive timeout.
    std::atomic<bool> finished{false};
};

struct Worker {
    std::shared_ptr<WorkerState> state;
    std::thread thread;
};

void validate(const HostConfig& config)
{
    if (config.bindEndpoint.empty()) {
        throw std::invalid_argument("NetworkServerHost bind endpoint is empty");
    }
    if (config.tickInterval.count() <= 0 || config.pollInterval.count() <= 0 ||
        config.workerStartTimeout.count() <= 0) {
        throw std::invalid_argument("NetworkServerHost intervals must be positive");
    }
    if (config.mode == HostMode::Dedicated &&
        Net::rewriteTcpEndpointHost("tcp://0.0.0.0:1", config.advertiseHost).empty()) {
        throw std::invalid_argument("NetworkServerHost advertise host is not usable");
    }
}

bool isAcceptedLeave(const Request& request, bool decoded, const Message& reply)
{
    if (!decoded || request.type != RequestType::Leave) {
        return false;
    }
    Reply decodedReply;
    std::string error;
    return decodeReply(reply, decodedReply, error) && decodedReply.type == ReplyType::Goodbye;
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

    // Server-owned platforms move on real time whether or not anybody is
    // asking, so they get a thread of their own rather than being advanced
    // only when a request happens to arrive.
    void runTicks()
    {
        while (running.load()) {
            server.update();
            std::this_thread::sleep_for(config.tickInterval);
        }
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
                Net::send(socket, server.handle(request));
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
            endpointReady.set_value(socket.get(zmq::sockopt::last_endpoint));

            while (state->alive.load() && running.load()) {
                Message requestMessage;
                bool received = false;
                try {
                    requestMessage = Net::receive(socket, received);
                } catch (const zmq::error_t&) {
                    continue;
                }
                if (!received) {
                    continue;
                }

                Request request;
                std::string error;
                const bool decoded = decodeRequest(requestMessage, request, error);
                const Message reply = server.handle(requestMessage);
                Net::send(socket, reply);

                // A malformed or unauthorized LEAVE produces ERROR. Keep this
                // worker alive unless the server actually accepted the departure.
                if (isAcceptedLeave(request, decoded, reply)) {
                    break;
                }
            }
        } catch (...) {
            try {
                endpointReady.set_value({});
            } catch (const std::future_error&) {
                // The endpoint was already published before the failure.
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
    bool rejoin(zmq::socket_t& handshake, const Request& request, const Message& requestMessage)
    {
        const std::lock_guard<std::mutex> lock(endpointsMutex);
        const auto existing = endpointsByToken.find(request.sessionToken);
        if (existing == endpointsByToken.end()) {
            return false;
        }

        const Message handled = server.handle(requestMessage);
        Reply welcome;
        std::string error;
        if (!decodeReply(handled, welcome, error) || welcome.type != ReplyType::Welcome) {
            Net::send(handshake, handled);
            return true;
        }
        Net::send(handshake, encodeWelcome(welcome.playerId, welcome.snapshot, existing->second.endpoint));
        return true;
    }

    void join(zmq::socket_t& handshake, const Request& request, const Message& requestMessage)
    {
        if (rejoin(handshake, request, requestMessage)) {
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
            Net::send(handshake, encodeError("could not start client worker"));
            return;
        }

        const Message handled = server.handle(requestMessage);
        Reply welcome;
        std::string error;
        if (!decodeReply(handled, welcome, error) || welcome.type != ReplyType::Welcome) {
            // Server full, for example. The worker was never announced.
            state->alive.store(false);
            Net::send(handshake, handled);
            return;
        }

        {
            const std::lock_guard<std::mutex> lock(endpointsMutex);
            endpointsByToken[request.sessionToken] = {sessionEndpoint, state};
        }
        log("player " + std::to_string(welcome.playerId) + " -> " + sessionEndpoint);
        Net::send(handshake, encodeWelcome(welcome.playerId, welcome.snapshot, sessionEndpoint));
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

            Request request;
            std::string error;
            if (!decodeRequest(requestMessage, request, error)) {
                Net::send(handshake, encodeError(error));
            } else if (request.type == RequestType::GetWorld) {
                Net::send(handshake, server.handle(requestMessage));
            } else if (request.type != RequestType::Join) {
                Net::send(handshake, encodeError("handshake accepts JOIN only"));
            } else {
                join(handshake, request, requestMessage);
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

std::size_t NetworkServerHost::activeWorkers() const
{
    const std::lock_guard<std::mutex> lock(impl_->workersMutex);
    return impl_->workers.size();
}

} // namespace Network
