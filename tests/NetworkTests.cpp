#include "NetworkProtocol.hpp"
#include "NetworkServer.hpp"
#include "TimeSource.hpp"

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

Network::Message receiveMessage(zmq::socket_t& socket)
{
    std::vector<zmq::message_t> raw;
    const auto result = zmq::recv_multipart(socket, std::back_inserter(raw));
    if (!result.has_value()) {
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

Network::Reply request(zmq::socket_t& client, zmq::socket_t& serverSocket,
                       Network::NetworkServer& server, const Network::Message& message, bool& passed)
{
    sendMessage(client, message);

    const Network::Message requestMessage = receiveMessage(serverSocket);
    passed &= expect(!requestMessage.empty(), "server should receive a TCP request");
    sendMessage(serverSocket, server.handle(requestMessage));

    Network::Reply reply;
    std::string error;
    passed &= expect(Network::decodeReply(receiveMessage(client), reply, error), "network reply should decode: " + error);
    return reply;
}

bool containsPlayer(const Network::WorldSnapshot& snapshot, Network::PlayerId playerId, float* x = nullptr)
{
    for (const Network::PlayerState& player : snapshot.players) {
        if (player.id == playerId) {
            if (x != nullptr) {
                *x = player.x;
            }
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    bool passed = true;

    Network::Request decoded;
    std::string error;
    passed &= expect(!Network::decodeRequest({"2", "JOIN"}, decoded, error), "unsupported version should fail");
    passed &= expect(!Network::decodeRequest({"1", "INPUT", "1", "1", "4", "0"}, decoded, error),
                     "movement outside -1..1 should fail");
    passed &= expect(!Network::decodeRequest({"1", "NOT_A_COMMAND"}, decoded, error), "unknown command should fail");

    ManualClock manualClock;
    Network::ServerConfig directConfig;
    directConfig.arenaWidth = 400.0F;
    directConfig.arenaHeight = 300.0F;
    directConfig.playerSize = 32.0F;
    directConfig.playerSpeed = 220.0F;
    directConfig.spawnPoints = {{48.0F, 48.0F}};
    directConfig.ticsPerSecond = 1000;
    directConfig.inactivityTimeoutTics = 3000;
    Network::NetworkServer directServer(manualClock, directConfig);

    Network::Reply joinReply;
    passed &= expect(Network::decodeReply(directServer.handle(Network::encodeJoin()), joinReply, error),
                     "JOIN reply should decode");
    passed &= expect(joinReply.type == Network::ReplyType::Welcome, "JOIN should receive WELCOME");
    const Network::PlayerId directId = joinReply.playerId;
    const float startX = joinReply.snapshot.players.front().x;

    Network::MovementInput moveRight{1, 0, 1};
    Network::Reply inputReply;
    passed &= expect(Network::decodeReply(directServer.handle(Network::encodeInput(directId, moveRight)), inputReply, error),
                     "INPUT reply should decode");
    manualClock.advance(1000);
    directServer.update();
    float movedX = 0.0F;
    passed &= expect(containsPlayer(directServer.snapshot(), directId, &movedX), "joined player should remain present");
    passed &= expect(std::fabs(movedX - (startX + directConfig.playerSpeed)) < 0.01F,
                     "server movement should use elapsed server time exactly");

    Network::Reply duplicateReply;
    Network::decodeReply(directServer.handle(Network::encodeInput(directId, moveRight)), duplicateReply, error);
    passed &= expect(duplicateReply.type == Network::ReplyType::Error, "duplicate input sequence should fail");
    Network::Reply unknownReply;
    Network::decodeReply(directServer.handle(Network::encodeLeave(9999)), unknownReply, error);
    passed &= expect(unknownReply.type == Network::ReplyType::Error, "unknown player should fail");

    zmq::context_t context(1);
    zmq::socket_t serverSocket(context, zmq::socket_type::rep);
    serverSocket.bind("tcp://127.0.0.1:*");
    const std::string endpoint = serverSocket.get(zmq::sockopt::last_endpoint);

    ManualClock networkClock;
    Network::ServerConfig networkConfig;
    networkConfig.arenaWidth = 400.0F;
    networkConfig.arenaHeight = 300.0F;
    networkConfig.playerSize = 32.0F;
    networkConfig.playerSpeed = 220.0F;
    networkConfig.spawnPoints = {{48.0F, 48.0F}, {160.0F, 48.0F}, {272.0F, 48.0F}};
    networkConfig.ticsPerSecond = 1000;
    networkConfig.inactivityTimeoutTics = 3000;
    Network::NetworkServer networkServer(networkClock, networkConfig);
    auto makeClient = [&] {
        zmq::socket_t client(context, zmq::socket_type::req);
        client.set(zmq::sockopt::rcvtimeo, 2000);
        client.set(zmq::sockopt::sndtimeo, 2000);
        client.connect(endpoint);
        return client;
    };

    zmq::socket_t clientOne = makeClient();
    zmq::socket_t clientTwo = makeClient();
    zmq::socket_t clientThree = makeClient();

    const Network::Reply firstJoin = request(clientOne, serverSocket, networkServer, Network::encodeJoin(), passed);
    const Network::Reply secondJoin = request(clientTwo, serverSocket, networkServer, Network::encodeJoin(), passed);
    passed &= expect(firstJoin.type == Network::ReplyType::Welcome && secondJoin.type == Network::ReplyType::Welcome &&
                         firstJoin.playerId != secondJoin.playerId,
                     "two clients should receive unique IDs");

    const Network::MovementInput firstMove{1, 0, 1};
    request(clientOne, serverSocket, networkServer, Network::encodeInput(firstJoin.playerId, firstMove), passed);
    networkClock.advance(1000);
    const Network::Reply secondSnapshot = request(clientTwo, serverSocket, networkServer,
                                                  Network::encodeInput(secondJoin.playerId, {0, 0, 1}), passed);
    float networkMovedX = 0.0F;
    passed &= expect(containsPlayer(secondSnapshot.snapshot, firstJoin.playerId, &networkMovedX),
                     "second client snapshot should include first player");
    passed &= expect(networkMovedX > firstJoin.snapshot.players.front().x,
                     "first client movement should appear to another client");

    const Network::Reply lateJoin = request(clientThree, serverSocket, networkServer, Network::encodeJoin(), passed);
    passed &= expect(lateJoin.type == Network::ReplyType::Welcome && lateJoin.snapshot.players.size() == 3,
                     "a third client can join after movement has started");

    // Client two stays within its heartbeat window, while client one does not.
    networkClock.advance(2500);
    const Network::Reply expirySnapshot = request(clientTwo, serverSocket, networkServer,
                                                  Network::encodeInput(secondJoin.playerId, {0, 0, 2}), passed);
    passed &= expect(!containsPlayer(expirySnapshot.snapshot, firstJoin.playerId),
                     "inactive client should disappear after timeout");

    return passed ? 0 : 1;
}
