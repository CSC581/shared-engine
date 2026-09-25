#include "NetworkServer.hpp"
#include "NetworkDemoConfig.hpp"
#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int)
{
    g_running.store(false);
}

// One blocking REP loop per connected client. A slow client only stalls this
// thread — other workers keep serving. Sockets stay on the worker thread
// (ZeroMQ sockets are not thread-safe).
void runClientWorker(zmq::context_t& context, Network::NetworkServer& server,
                     std::promise<std::string> endpointReady,
                     std::shared_ptr<std::atomic<bool>> alive,
                     Network::SessionToken sessionToken,
                     std::mutex& endpointsMutex,
                     std::unordered_map<Network::SessionToken, std::string>& endpointsByToken)
{
    try {
        zmq::socket_t socket(context, zmq::socket_type::rep);
        socket.set(zmq::sockopt::linger, 0);
        // Wake periodically so a cancelled worker (failed JOIN / shutdown) can exit.
        socket.set(zmq::sockopt::rcvtimeo, 500);
        // Bind on all interfaces; main rewrites the host for WELCOME via --advertise.
        socket.bind("tcp://0.0.0.0:*");
        const std::string endpoint = socket.get(zmq::sockopt::last_endpoint);
        endpointReady.set_value(endpoint);

        while (alive->load() && g_running.load()) {
            Network::Message requestMessage;
            bool received = false;
            try {
                requestMessage = Net::receive(socket, received);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (!received) {
                continue;
            }

            Network::Request request;
            std::string error;
            const bool decoded = Network::decodeRequest(requestMessage, request, error);
            const Network::Message reply = server.handle(requestMessage);
            Net::send(socket, reply);

            if (decoded && request.type == Network::RequestType::Leave) {
                break;
            }
        }
    } catch (...) {
        try {
            endpointReady.set_value({});
        } catch (const std::future_error&) {
            // Endpoint was already published before the failure.
        }
    }

    {
        const std::lock_guard<std::mutex> lock(endpointsMutex);
        endpointsByToken.erase(sessionToken);
    }
    alive->store(false);
}

struct ClientWorker {
    std::shared_ptr<std::atomic<bool>> alive;
    std::thread thread;
};

} // namespace

int main(int argc, char* argv[])
{
    std::string handshakeEndpoint = "tcp://*:5555";
    std::string advertiseHost = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--advertise") {
            if (i + 1 >= argc) {
                std::cerr << "network-server: --advertise requires a host\n";
                return 1;
            }
            advertiseHost = argv[++i];
            continue;
        }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "network-server: unknown option " << arg << '\n';
            return 1;
        }
        handshakeEndpoint = arg;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    int exitCode = 0;
    zmq::context_t context(1);
    RealTimeClock clock;
    Network::NetworkServer server(clock, NetworkDemo::makeServerConfig());

    std::mutex workersMutex;
    std::vector<ClientWorker> workers;
    std::thread platformThread;
    std::mutex endpointsMutex;
    std::unordered_map<Network::SessionToken, std::string> endpointsByToken;

    try {
        zmq::socket_t handshake(context, zmq::socket_type::rep);
        handshake.set(zmq::sockopt::linger, 0);
        // Poll so Ctrl-C / error shutdown can leave the accept loop.
        handshake.set(zmq::sockopt::rcvtimeo, 500);
        handshake.bind(handshakeEndpoint);

        platformThread = std::thread([&server] {
            while (g_running.load()) {
                server.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });

        std::cout << "Network server handshake listening on " << handshakeEndpoint << '\n';
        std::cout << "Session endpoints advertise host " << advertiseHost << '\n';
        std::cout << "Each JOIN spawns a dedicated per-client REP worker (no Router/Dealer).\n";
        std::cout << "Moving platforms are server-authored on real time.\n";

        while (g_running.load()) {
            Network::Message requestMessage;
            bool received = false;
            try {
                requestMessage = Net::receive(handshake, received);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (!received) {
                continue;
            }

            Network::Request request;
            std::string error;
            if (!Network::decodeRequest(requestMessage, request, error)) {
                Net::send(handshake, Network::encodeError(error));
                continue;
            }

            if (request.type != Network::RequestType::Join) {
                Net::send(handshake, Network::encodeError("handshake accepts JOIN only"));
                continue;
            }

            // Rejoin: reuse the existing worker endpoint when this token is live.
            {
                const std::lock_guard<std::mutex> lock(endpointsMutex);
                const auto existing = endpointsByToken.find(request.sessionToken);
                if (existing != endpointsByToken.end()) {
                    const Network::Message handled = server.handle(requestMessage);
                    Network::Reply welcome;
                    std::string welcomeError;
                    if (!Network::decodeReply(handled, welcome, welcomeError) ||
                        welcome.type != Network::ReplyType::Welcome) {
                        Net::send(handshake, handled);
                        continue;
                    }
                    Net::send(handshake, Network::encodeWelcome(welcome.playerId, welcome.snapshot,
                                                                existing->second));
                    continue;
                }
            }

            auto alive = std::make_shared<std::atomic<bool>>(true);
            std::promise<std::string> endpointReady;
            std::future<std::string> endpointFuture = endpointReady.get_future();
            const Network::SessionToken token = request.sessionToken;

            ClientWorker worker;
            worker.alive = alive;
            worker.thread = std::thread(runClientWorker, std::ref(context), std::ref(server),
                                        std::move(endpointReady), alive, token, std::ref(endpointsMutex),
                                        std::ref(endpointsByToken));
            {
                const std::lock_guard<std::mutex> lock(workersMutex);
                workers.push_back(std::move(worker));
            }

            const std::string boundEndpoint = endpointFuture.get();
            const std::string sessionEndpoint =
                Network::rewriteTcpEndpointHost(boundEndpoint, advertiseHost);
            if (boundEndpoint.empty() || sessionEndpoint.empty()) {
                alive->store(false);
                Net::send(handshake, Network::encodeError("could not start client worker"));
                continue;
            }

            const Network::Message handled = server.handle(requestMessage);
            Network::Reply welcome;
            std::string welcomeError;
            if (!Network::decodeReply(handled, welcome, welcomeError) ||
                welcome.type != Network::ReplyType::Welcome) {
                alive->store(false);
                Net::send(handshake, handled);
                continue;
            }

            {
                const std::lock_guard<std::mutex> lock(endpointsMutex);
                endpointsByToken[token] = sessionEndpoint;
            }

            std::cout << "Client player " << welcome.playerId << " -> " << sessionEndpoint << '\n';
            Net::send(handshake,
                      Network::encodeWelcome(welcome.playerId, welcome.snapshot, sessionEndpoint));
        }
    } catch (const std::exception& exception) {
        std::cerr << "Network server error: " << exception.what() << '\n';
        exitCode = 1;
    }

    // Stop background work while server/context are still alive (no detached UAF).
    g_running.store(false);
    {
        const std::lock_guard<std::mutex> lock(workersMutex);
        for (ClientWorker& worker : workers) {
            if (worker.alive) {
                worker.alive->store(false);
            }
        }
    }
    try {
        context.shutdown();
    } catch (const zmq::error_t&) {
        // Already shut down or closing.
    }

    if (platformThread.joinable()) {
        platformThread.join();
    }
    {
        const std::lock_guard<std::mutex> lock(workersMutex);
        for (ClientWorker& worker : workers) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
        }
        workers.clear();
    }

    return exitCode;
}
