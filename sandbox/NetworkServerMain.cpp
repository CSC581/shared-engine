#include "NetworkServer.hpp"
#include "NetworkDemoConfig.hpp"
#include "NetworkProtocol.hpp"
#include "TimeSource.hpp"

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

Network::Message receiveMessage(zmq::socket_t& socket)
{
    std::vector<zmq::message_t> raw;
    const auto received = zmq::recv_multipart(socket, std::back_inserter(raw));
    if (!received.has_value()) {
        return {};
    }

    Network::Message message;
    message.reserve(raw.size());
    for (const zmq::message_t& field : raw) {
        message.push_back(field.to_string());
    }
    return message;
}

void sendMessage(zmq::socket_t& socket, const Network::Message& message)
{
    std::vector<zmq::const_buffer> buffers;
    buffers.reserve(message.size());
    for (const std::string& field : message) {
        buffers.push_back(zmq::buffer(field));
    }
    zmq::send_multipart(socket, buffers);
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
        // Wake periodically so a cancelled worker (failed JOIN) can exit.
        socket.set(zmq::sockopt::rcvtimeo, 500);
        // Bind on loopback with an ephemeral port so the welcome can advertise a
        // concrete address clients on this machine can connect to.
        socket.bind("tcp://127.0.0.1:*");
        const std::string endpoint = socket.get(zmq::sockopt::last_endpoint);
        endpointReady.set_value(endpoint);

        while (alive->load()) {
            Network::Message requestMessage;
            try {
                requestMessage = receiveMessage(socket);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (requestMessage.empty()) {
                continue;
            }

            Network::Request request;
            std::string error;
            const bool decoded = Network::decodeRequest(requestMessage, request, error);
            const Network::Message reply = server.handle(requestMessage);
            sendMessage(socket, reply);

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

} // namespace

int main(int argc, char* argv[])
{
    const std::string handshakeEndpoint = argc > 1 ? argv[1] : "tcp://*:5555";

    try {
        zmq::context_t context(1);
        zmq::socket_t handshake(context, zmq::socket_type::rep);
        handshake.set(zmq::sockopt::linger, 0);
        handshake.bind(handshakeEndpoint);

        RealTimeClock clock;
        Network::NetworkServer server(clock, NetworkDemo::makeServerConfig());

        // Keep platforms moving on real time even when no client is mid-request.
        std::thread([&server] {
            while (true) {
                server.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }).detach();

        std::mutex endpointsMutex;
        std::unordered_map<Network::SessionToken, std::string> endpointsByToken;

        std::cout << "Network server handshake listening on " << handshakeEndpoint << '\n';
        std::cout << "Each JOIN spawns a dedicated per-client REP worker (no Router/Dealer).\n";
        std::cout << "Moving platforms are server-authored on real time.\n";

        while (true) {
            const Network::Message requestMessage = receiveMessage(handshake);
            if (requestMessage.empty()) {
                continue;
            }

            Network::Request request;
            std::string error;
            if (!Network::decodeRequest(requestMessage, request, error)) {
                sendMessage(handshake, Network::encodeError(error));
                continue;
            }

            if (request.type != Network::RequestType::Join) {
                sendMessage(handshake, Network::encodeError("handshake accepts JOIN only"));
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
                        sendMessage(handshake, handled);
                        continue;
                    }
                    sendMessage(handshake,
                                Network::encodeWelcome(welcome.playerId, welcome.snapshot, existing->second));
                    continue;
                }
            }

            auto alive = std::make_shared<std::atomic<bool>>(true);
            std::promise<std::string> endpointReady;
            std::future<std::string> endpointFuture = endpointReady.get_future();
            const Network::SessionToken token = request.sessionToken;

            std::thread(runClientWorker, std::ref(context), std::ref(server), std::move(endpointReady), alive,
                        token, std::ref(endpointsMutex), std::ref(endpointsByToken))
                .detach();

            const std::string sessionEndpoint = endpointFuture.get();
            if (sessionEndpoint.empty()) {
                sendMessage(handshake, Network::encodeError("could not start client worker"));
                continue;
            }

            const Network::Message handled = server.handle(requestMessage);
            Network::Reply welcome;
            std::string welcomeError;
            if (!Network::decodeReply(handled, welcome, welcomeError) ||
                welcome.type != Network::ReplyType::Welcome) {
                alive->store(false);
                sendMessage(handshake, handled);
                continue;
            }

            {
                const std::lock_guard<std::mutex> lock(endpointsMutex);
                endpointsByToken[token] = sessionEndpoint;
            }

            std::cout << "Client player " << welcome.playerId << " -> " << sessionEndpoint << '\n';
            sendMessage(handshake,
                        Network::encodeWelcome(welcome.playerId, welcome.snapshot, sessionEndpoint));
        }
    } catch (const std::exception& exception) {
        std::cerr << "Network server error: " << exception.what() << '\n';
        return 1;
    }
}
