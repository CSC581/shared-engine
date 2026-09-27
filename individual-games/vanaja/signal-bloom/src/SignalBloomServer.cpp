#include "SignalBloomWorld.hpp"

#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <future>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> running{true};
std::mutex outputMutex;

void stopServer(int)
{
    running.store(false);
}

struct Worker {
    std::shared_ptr<std::atomic<bool>> alive;
    std::thread thread;
};

void clientWorker(zmq::context_t& context, Network::NetworkServer& server,
                  std::promise<std::string> endpointReady,
                  std::shared_ptr<std::atomic<bool>> alive,
                  Network::SessionToken token, std::mutex& endpointMutex,
                  std::unordered_map<Network::SessionToken, std::string>& endpoints)
{
    try {
        zmq::socket_t socket(context, zmq::socket_type::rep);
        socket.set(zmq::sockopt::linger, 0);
        socket.set(zmq::sockopt::rcvtimeo, 250);
        socket.bind("tcp://0.0.0.0:*");
        endpointReady.set_value(socket.get(zmq::sockopt::last_endpoint));

        auto sampleStart = std::chrono::steady_clock::now();
        std::uint32_t accepted = 0;
        Network::PlayerId playerId = 0;
        while (alive->load() && running.load()) {
            bool received = false;
            Network::Message wire;
            try {
                wire = Net::receive(socket, received);
            } catch (const zmq::error_t&) {
                if (!running.load()) break;
                continue;
            }

            if (received) {
                Network::Request request;
                std::string error;
                const bool decoded = Network::decodeRequest(wire, request, error);
                const Network::Message reply = server.handle(wire);
                Net::send(socket, reply);

                Network::Reply parsedReply;
                std::string replyError;
                const bool replyDecoded = Network::decodeReply(reply, parsedReply, replyError);
                if (decoded && request.type == Network::RequestType::Position &&
                    replyDecoded && parsedReply.type == Network::ReplyType::Snapshot) {
                    playerId = request.playerId;
                    ++accepted;
                }
                if (decoded && request.type == Network::RequestType::Leave &&
                    replyDecoded && parsedReply.type == Network::ReplyType::Goodbye) {
                    break;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<double>(now - sampleStart).count();
            if (elapsed >= 1.0) {
                if (playerId != 0) {
                    const std::lock_guard<std::mutex> lock(outputMutex);
                    std::cout << "player " << playerId << ": "
                              << static_cast<double>(accepted) / elapsed
                              << " accepted POSITION/s\n";
                }
                sampleStart = now;
                accepted = 0;
            }
        }
    } catch (...) {
        try {
            endpointReady.set_value({});
        } catch (const std::future_error&) {
            // The endpoint has already been given to the accept loop.
        }
    }

    {
        const std::lock_guard<std::mutex> lock(endpointMutex);
        endpoints.erase(token);
    }
    alive->store(false);
}

} // namespace

int main(int argc, char* argv[])
{
    std::string bind = "tcp://*:5555";
    std::string advertise = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--bind" || arg == "--advertise") && i + 1 < argc) {
            (arg == "--bind" ? bind : advertise) = argv[++i];
        } else {
            std::cerr << "Usage: signal-bloom-server [--bind tcp://*:5555] "
                         "[--advertise reachable-host]\n";
            return 1;
        }
    }

    if (advertise.empty() || advertise == "0.0.0.0" || advertise == "*") {
        std::cerr << "--advertise must be an address the clients can reach\n";
        return 1;
    }

    std::signal(SIGINT, stopServer);
    std::signal(SIGTERM, stopServer);

    zmq::context_t context(1);
    RealTimeClock clock;
    Network::NetworkServer server(clock, SignalBloom::serverConfig());
    std::mutex endpointMutex;
    std::unordered_map<Network::SessionToken, std::string> endpoints;
    std::vector<Worker> workers;
    std::thread platformThread;
    int result = 0;

    try {
        zmq::socket_t handshake(context, zmq::socket_type::rep);
        handshake.set(zmq::sockopt::linger, 0);
        handshake.set(zmq::sockopt::rcvtimeo, 250);
        handshake.bind(bind);

        platformThread = std::thread([&server] {
            while (running.load()) {
                server.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });

        std::cout << "Signal Bloom world listening on " << bind << '\n'
                  << "Private client workers advertise " << advertise << '\n'
                  << "GET_WORLD reads shuttles only; POSITION/s reports accepted client updates.\n";

        while (running.load()) {
            bool received = false;
            Network::Message wire;
            try {
                wire = Net::receive(handshake, received);
            } catch (const zmq::error_t&) {
                if (!running.load()) break;
                continue;
            }
            if (!received) continue;

            Network::Request request;
            std::string error;
            if (!Network::decodeRequest(wire, request, error)) {
                Net::send(handshake, Network::encodeError(error));
                continue;
            }
            if (request.type == Network::RequestType::GetWorld) {
                Net::send(handshake, server.handle(wire));
                continue;
            }
            if (request.type != Network::RequestType::Join) {
                Net::send(handshake, Network::encodeError("handshake accepts JOIN or GET_WORLD only"));
                continue;
            }

            {
                const std::lock_guard<std::mutex> lock(endpointMutex);
                const auto existing = endpoints.find(request.sessionToken);
                if (existing != endpoints.end()) {
                    const Network::Message reply = server.handle(wire);
                    Network::Reply welcome;
                    std::string replyError;
                    if (Network::decodeReply(reply, welcome, replyError) &&
                        welcome.type == Network::ReplyType::Welcome) {
                        Net::send(handshake, Network::encodeWelcome(
                            welcome.playerId, welcome.snapshot, existing->second));
                    } else {
                        Net::send(handshake, reply);
                    }
                    continue;
                }
            }

            Worker worker;
            worker.alive = std::make_shared<std::atomic<bool>>(true);
            std::promise<std::string> ready;
            auto future = ready.get_future();
            worker.thread = std::thread(clientWorker, std::ref(context), std::ref(server),
                                        std::move(ready), worker.alive, request.sessionToken,
                                        std::ref(endpointMutex), std::ref(endpoints));
            workers.push_back(std::move(worker));

            const std::string bound = future.get();
            const std::string privateEndpoint =
                Network::rewriteTcpEndpointHost(bound, advertise);
            if (bound.empty() || privateEndpoint.empty()) {
                workers.back().alive->store(false);
                Net::send(handshake, Network::encodeError("could not start client worker"));
                continue;
            }

            const Network::Message reply = server.handle(wire);
            Network::Reply welcome;
            std::string replyError;
            if (!Network::decodeReply(reply, welcome, replyError) ||
                welcome.type != Network::ReplyType::Welcome) {
                workers.back().alive->store(false);
                Net::send(handshake, reply);
                continue;
            }
            {
                const std::lock_guard<std::mutex> lock(endpointMutex);
                endpoints[request.sessionToken] = privateEndpoint;
            }
            std::cout << "player " << welcome.playerId << " joined via " << privateEndpoint << '\n';
            Net::send(handshake, Network::encodeWelcome(
                welcome.playerId, welcome.snapshot, privateEndpoint));
        }
    } catch (const std::exception& exception) {
        std::cerr << "Signal Bloom server: " << exception.what() << '\n';
        result = 1;
    }

    running.store(false);
    for (Worker& worker : workers) worker.alive->store(false);
    try {
        context.shutdown();
    } catch (const zmq::error_t&) {
    }
    if (platformThread.joinable()) platformThread.join();
    for (Worker& worker : workers) {
        if (worker.thread.joinable()) worker.thread.join();
    }
    return result;
}
