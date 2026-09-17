#include "NetworkClient.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkServer.hpp"
#include "TimeSource.hpp"
#include "../sandbox/NetworkDemoPlayer.hpp"

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <thread>
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

class CommaDecimal final : public std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};

bool rejectsIncompatibleReply(const Network::Message& message)
{
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, 2000);
    socket.set(zmq::sockopt::sndtimeo, 2000);
    socket.bind("tcp://127.0.0.1:*");
    ManualClock clock;
    Network::NetworkClient client(clock, socket.get(zmq::sockopt::last_endpoint));
    client.start();
    if (!expect(!receiveMessage(socket).empty(), "test server should receive JOIN")) {
        return false;
    }
    sendMessage(socket, message);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.state() != Network::ConnectionState::Error && std::chrono::steady_clock::now() < deadline) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool passed = expect(client.state() == Network::ConnectionState::Error &&
                             client.error().find("Rebuild and restart") != std::string::npos,
                         "version mismatch should produce an actionable terminal error");
    clock.advance(10 * kNsPerSec);
    client.poll();
    passed &= expect(client.state() == Network::ConnectionState::Error && client.playerId() == 0 &&
                         client.snapshot().players.empty(),
                     "polling after a terminal error must not reconnect or retain a stale world");
    // Explicit retry remains possible once the endpoint is serving compatible replies.
    client.start();
    if (!expect(!receiveMessage(socket).empty(), "explicit retry should send a new JOIN")) {
        return false;
    }
    sendMessage(socket, Network::encodeWelcome(1, {1, {{1, 10, 20}}}));
    const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.state() != Network::ConnectionState::Connected && std::chrono::steady_clock::now() < retryDeadline) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    passed &= expect(client.state() == Network::ConnectionState::Connected && client.error().empty(),
                     "explicit retry should recover when a compatible server responds");
    return passed;
}

bool clearsWorldWhileRetrying()
{
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, 2000);
    socket.set(zmq::sockopt::sndtimeo, 2000);
    socket.bind("tcp://127.0.0.1:*");

    ManualClock clock;
    Network::NetworkClient client(clock, socket.get(zmq::sockopt::last_endpoint));
    client.start();
    if (!expect(!receiveMessage(socket).empty(), "test server should receive initial JOIN")) {
        return false;
    }
    sendMessage(socket, Network::encodeWelcome(7, {4, {{7, 10, 20}, {8, 30, 40}}}));

    const auto connectedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.state() != Network::ConnectionState::Connected && std::chrono::steady_clock::now() < connectedDeadline) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!expect(client.state() == Network::ConnectionState::Connected && client.snapshot().players.size() == 2,
                "test client should hold a populated world before retrying")) {
        return false;
    }

    client.submitPosition(15.0F, 25.0F);
    if (!expect(!receiveMessage(socket).empty(), "test server should receive POSITION")) {
        return false;
    }
    sendMessage(socket, Network::encodeError("temporary server problem"));

    const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.state() != Network::ConnectionState::Connecting && std::chrono::steady_clock::now() < retryDeadline) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return expect(client.state() == Network::ConnectionState::Connecting && client.playerId() == 0 &&
                      client.snapshot().players.empty(),
                  "a retrying client must clear its stale player ID and world snapshot");
}

} // namespace

int main()
{
    bool passed = true;
    const Network::SessionToken tokenOne(32, '1');
    const Network::SessionToken tokenTwo(32, '2');
    const Network::SessionToken tokenThree(32, '3');

    try {
        Network::NetworkClient invalidClient("hello");
        invalidClient.start();
        invalidClient.poll();
        passed &= expect(invalidClient.state() == Network::ConnectionState::Error &&
                             invalidClient.error().find("server address") != std::string::npos,
                         "invalid endpoint should report an error without throwing");
        invalidClient.leave();
    } catch (const std::exception& exception) {
        passed &= expect(false, std::string("invalid endpoint escaped the client: ") + exception.what());
    }
    passed &= rejectsIncompatibleReply({"2", "ERROR", "unsupported protocol version"});
    passed &= rejectsIncompatibleReply(Network::encodeError("unsupported protocol version"));
    passed &= clearsWorldWhileRetrying();

    Network::Request decoded;
    std::string error;
    const std::string version = std::to_string(Network::protocolVersion);

    // Locale changes happen before the later threaded tests. Test both C++
    // punctuation and, where installed, the C locale used by the old stof path.
    const std::locale savedCppLocale = std::locale();
    const std::string savedNumericLocale = std::setlocale(LC_NUMERIC, nullptr);
    std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    if (!std::setlocale(LC_NUMERIC, "fr_FR.UTF-8")) {
        std::cout << "French C locale unavailable; testing custom C++ comma locale only\n";
    }
    const auto localePosition = Network::encodePosition(1, tokenOne, {12.5F, 3000.25F, 1});
    passed &= expect(localePosition[5] == "12.5" && localePosition[6] == "3000.25",
                     "wire coordinates should always use dots and no thousands separators");
    passed &= expect(Network::decodeRequest({version, "POSITION", "1", tokenOne, "1", "12.5", "3000.25"}, decoded, error) &&
                         decoded.position.x == 12.5F && decoded.position.y == 3000.25F,
                     "dot-decimal messages should parse even under a comma locale");
    Network::Reply localeReply;
    const auto localeSnapshot = Network::encodeSnapshot({1, {{1, 12.5F, 3000.25F}}});
    std::locale::global(savedCppLocale);
    std::setlocale(LC_NUMERIC, "C");
    passed &= expect(Network::decodeRequest(localePosition, decoded, error) && decoded.position.x == 12.5F,
                     "a receiver using C locale should accept a message encoded under a comma locale");
    passed &= expect(Network::decodeReply(localeSnapshot, localeReply, error) &&
                         localeReply.snapshot.players.front().y == 3000.25F,
                     "server snapshot coordinates must also be locale-independent");
    for (const float coordinate : {std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
                                   std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min(),
                                   std::nextafter(1.0F, 2.0F), -0.0F}) {
        const auto message = Network::encodePosition(1, tokenOne, {coordinate, coordinate, 1});
        passed &= expect(Network::decodeRequest(message, decoded, error) && decoded.position.x == coordinate,
                         "finite float coordinate should round trip without precision loss: " + message[5]);
    }
    passed &= expect(!Network::decodeRequest({version, "POSITION", "1", tokenOne, "1", "12,5", "0"}, decoded, error),
                     "comma-decimal wire coordinates must be rejected consistently");
    std::setlocale(LC_NUMERIC, savedNumericLocale.c_str());
    passed &= expect(!Network::decodeRequest({"99", "JOIN", tokenOne}, decoded, error), "unsupported version should fail");
    passed &= expect(!Network::decodeRequest({"2", "JOIN", tokenOne}, decoded, error),
                     "previous direction-based protocol should be rejected");
    passed &= expect(!Network::decodeRequest({version, "JOIN", "not-a-token"}, decoded, error) &&
                         error == "JOIN requires a valid session token",
                     "invalid session token should fail");
    passed &= expect(!Network::decodeRequest({version, "NOT_A_COMMAND", "1", tokenOne}, decoded, error) &&
                         error == "unknown request command", "unknown command should fail");
    for (const std::string coordinate : {"nan", "inf", "-inf", "12oops"}) {
        passed &= expect(!Network::decodeRequest({version, "POSITION", "1", tokenOne, "1", coordinate, "0"},
                                                 decoded, error) && error == "invalid position update",
                         "invalid x position should fail");
        passed &= expect(!Network::decodeRequest({version, "POSITION", "1", tokenOne, "1", "0", coordinate},
                                                 decoded, error), "invalid y position should fail");
    }
    for (const std::string sequence : {"0", "-1", "18446744073709551616"}) {
        passed &= expect(!Network::decodeRequest({version, "POSITION", "1", tokenOne, sequence, "1", "2"},
                                                 decoded, error), "invalid sequence should fail");
    }
    passed &= expect(!Network::decodeRequest({version, "POSITION", "1", tokenOne, "1", "5"}, decoded, error),
                     "position messages require both coordinates");
    passed &= expect(Network::decodeRequest(Network::encodePosition(1, tokenOne, {-12.5F, 800.25F, 1}),
                                            decoded, error) && decoded.type == Network::RequestType::Position &&
                         decoded.position.x == -12.5F && decoded.position.y == 800.25F,
                     "position messages should round trip without imposing demo bounds");

    // Exercise the same local simulation the SDL demo uses, without a window.
    NetworkDemo::Player localPlayer;
    const Network::WorldSnapshot initial{1, {{1, 48.0F, 48.0F}}};
    passed &= expect(localPlayer.synchronize(initial, 1), "local player should initialize from its join snapshot");
    localPlayer.update(1, 0, 0.5F);
    const float localX = 48.0F + NetworkDemo::playerSpeed * 0.5F;
    passed &= expect(std::fabs(localPlayer.bounds().x - localX) < 0.01F,
                     "client movement should use elapsed game time");
    localPlayer.synchronize(initial, 1);
    passed &= expect(localPlayer.bounds().x == localX, "old snapshot echoes must not rewind local movement");
    localPlayer.update(1, 0, 0.0F);
    passed &= expect(localPlayer.bounds().x == localX, "zero game delta should freeze local movement");
    localPlayer.reset();
    localPlayer.synchronize(initial, 1);
    localPlayer.update(1, 1, 0.5F);
    passed &= expect(std::fabs(std::hypot(localPlayer.bounds().x - 48.0F, localPlayer.bounds().y - 48.0F) -
                              NetworkDemo::playerSpeed * 0.5F) < 0.01F,
                     "diagonal local movement should not be faster");
    localPlayer.update(1, 1, 100.0F);
    passed &= expect(localPlayer.bounds().x == NetworkDemo::arenaWidth - NetworkDemo::playerSize &&
                         localPlayer.bounds().y == NetworkDemo::arenaHeight - NetworkDemo::playerSize,
                     "client should enforce demo arena bounds");
    localPlayer.reset();
    localPlayer.synchronize(initial, 1);
    passed &= expect(localPlayer.bounds().x == 48.0F, "rejoining should adopt the stored position again");

    ManualClock manualClock;
    Network::ServerConfig directConfig;
    directConfig.spawnPoints = {{48.0F, 48.0F}};
    directConfig.inactivityTimeoutTics = 3000;
    Network::NetworkServer directServer(manualClock, directConfig);

    Network::Reply joinReply;
    passed &= expect(Network::decodeReply(directServer.handle(Network::encodeJoin(tokenOne)), joinReply, error),
                     "JOIN reply should decode");
    passed &= expect(joinReply.type == Network::ReplyType::Welcome, "JOIN should receive WELCOME");
    const Network::PlayerId directId = joinReply.playerId;
    const Network::PositionUpdate position{-12.5F, 800.25F, 1};
    Network::Reply positionReply;
    passed &= expect(Network::decodeReply(directServer.handle(Network::encodePosition(directId, tokenOne, position)), positionReply, error) &&
                         positionReply.type == Network::ReplyType::Snapshot,
                     "POSITION should receive a snapshot");
    const auto revision = directServer.snapshot().serverTick;
    manualClock.advance(1000);
    directServer.update();
    float movedX = 0.0F;
    passed &= expect(containsPlayer(directServer.snapshot(), directId, &movedX), "joined player should remain present");
    passed &= expect(movedX == position.x && directServer.snapshot().players.front().y == position.y &&
                         directServer.snapshot().serverTick == revision,
                     "server should preserve reported coordinates without integrating or clamping them");

    Network::Reply duplicateReply;
    Network::decodeReply(directServer.handle(Network::encodePosition(directId, tokenOne, {900, 900, 1})), duplicateReply, error);
    passed &= expect(duplicateReply.type == Network::ReplyType::Error, "duplicate position sequence should fail");
    Network::Reply wrongOwnerReply;
    Network::decodeReply(directServer.handle(Network::encodePosition(directId, tokenTwo, {0, 0, 2})), wrongOwnerReply, error);
    passed &= expect(wrongOwnerReply.type == Network::ReplyType::Error,
                     "another session must not control this player");
    passed &= expect(directServer.snapshot().players.front().x == position.x &&
                         directServer.snapshot().serverTick == revision,
                     "rejected updates must not overwrite the stored position or revision");
    Network::Reply repeatedJoinReply;
    Network::decodeReply(directServer.handle(Network::encodeJoin(tokenOne)), repeatedJoinReply, error);
    passed &= expect(repeatedJoinReply.type == Network::ReplyType::Welcome &&
                         repeatedJoinReply.playerId == directId && directServer.playerCount() == 1,
                     "repeated JOIN should reuse the same player");
    Network::Reply unknownReply;
    Network::decodeReply(directServer.handle(Network::encodeLeave(9999, tokenOne)), unknownReply, error);
    passed &= expect(unknownReply.type == Network::ReplyType::Error, "unknown player should fail");

    Network::ServerConfig spawnConfig;
    spawnConfig.spawnPoints = {{10.0F, 20.0F}, {30.0F, 40.0F}};
    Network::NetworkServer spawnServer(manualClock, spawnConfig);
    Network::Reply spawnFirst;
    Network::Reply spawnSecond;
    Network::Reply spawnThird;
    const Network::SessionToken tokenFour(32, '4');
    const Network::SessionToken tokenFive(32, '5');
    Network::decodeReply(spawnServer.handle(Network::encodeJoin(tokenOne)), spawnFirst, error);
    Network::decodeReply(spawnServer.handle(Network::encodeJoin(tokenTwo)), spawnSecond, error);
    Network::decodeReply(spawnServer.handle(Network::encodeLeave(spawnSecond.playerId, tokenTwo)), unknownReply, error);
    Network::decodeReply(spawnServer.handle(Network::encodeJoin(tokenFour)), spawnThird, error);
    passed &= expect(spawnServer.playerCount() == 2 &&
                         spawnThird.snapshot.players[0].x != spawnThird.snapshot.players[1].x,
                     "a join after another player leaves should use the available spawn point");
    Network::Reply spawnFourth;
    Network::decodeReply(spawnServer.handle(Network::encodeJoin(tokenFive)), spawnFourth, error);
    passed &= expect(spawnFourth.type == Network::ReplyType::Welcome && spawnServer.playerCount() == 3,
                     "joining remains possible when every configured spawn point is occupied");

    zmq::context_t context(1);
    zmq::socket_t serverSocket(context, zmq::socket_type::rep);
    serverSocket.bind("tcp://127.0.0.1:*");
    const std::string endpoint = serverSocket.get(zmq::sockopt::last_endpoint);

    ManualClock networkClock;
    Network::ServerConfig networkConfig;
    networkConfig.spawnPoints = {{48.0F, 48.0F}, {160.0F, 48.0F}, {272.0F, 48.0F}};
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

    const Network::Reply firstJoin = request(clientOne, serverSocket, networkServer, Network::encodeJoin(tokenOne), passed);
    const Network::Reply secondJoin = request(clientTwo, serverSocket, networkServer, Network::encodeJoin(tokenTwo), passed);
    passed &= expect(firstJoin.type == Network::ReplyType::Welcome && secondJoin.type == Network::ReplyType::Welcome &&
                         firstJoin.playerId != secondJoin.playerId,
                     "two clients should receive unique IDs");

    const Network::PositionUpdate firstPosition{250.5F, 125.25F, 1};
    request(clientOne, serverSocket, networkServer, Network::encodePosition(firstJoin.playerId, tokenOne, firstPosition), passed);
    networkClock.advance(1000);
    const Network::Reply secondSnapshot = request(clientTwo, serverSocket, networkServer,
                                                  Network::encodePosition(secondJoin.playerId, tokenTwo, {160, 48, 1}), passed);
    float networkMovedX = 0.0F;
    passed &= expect(containsPlayer(secondSnapshot.snapshot, firstJoin.playerId, &networkMovedX),
                     "second client snapshot should include first player");
    passed &= expect(secondSnapshot.type == Network::ReplyType::Snapshot && networkMovedX == firstPosition.x,
                     "another client should receive exactly the reported position");

    const Network::Reply lateJoin = request(clientThree, serverSocket, networkServer, Network::encodeJoin(tokenThree), passed);
    passed &= expect(lateJoin.type == Network::ReplyType::Welcome && lateJoin.snapshot.players.size() == 3,
                     "a third client can join after movement has started");

    // Client two stays within its heartbeat window, while client one does not.
    networkClock.advance(2500);
    const Network::Reply expirySnapshot = request(clientTwo, serverSocket, networkServer,
                                                  Network::encodePosition(secondJoin.playerId, tokenTwo, {160, 48, 2}), passed);
    passed &= expect(expirySnapshot.type == Network::ReplyType::Snapshot &&
                         containsPlayer(expirySnapshot.snapshot, secondJoin.playerId) &&
                         !containsPlayer(expirySnapshot.snapshot, firstJoin.playerId),
                     "inactive client should disappear after timeout");

    // Start an actual NetworkClient before its server exists. It must keep its
    // update loop non-blocking, then join when the server becomes available.
    zmq::socket_t reservation(context, zmq::socket_type::rep);
    reservation.bind("tcp://127.0.0.1:*");
    const std::string delayedEndpoint = reservation.get(zmq::sockopt::last_endpoint);
    reservation.close();

    ManualClock clientClock;
    Network::NetworkClient delayedClient(clientClock, delayedEndpoint);
    delayedClient.start();
    passed &= expect(delayedClient.error().empty(), "initial JOIN should be queued");
    clientClock.advance(kNsPerSec - 1);
    delayedClient.poll();
    passed &= expect(delayedClient.error().empty(), "request must not time out before one second");
    clientClock.advance(1);
    delayedClient.poll();
    passed &= expect(delayedClient.error() == "waiting for server at " + delayedEndpoint,
                     "request should time out exactly at one second on the supplied clock");
    passed &= expect(delayedClient.state() == Network::ConnectionState::Connecting,
                     "client without a server should remain connecting");

    std::atomic<bool> delayedServerRunning{true};
    std::promise<void> delayedServerBound;
    std::future<void> delayedServerReady = delayedServerBound.get_future();
    std::thread delayedServerThread([&] {
        zmq::context_t delayedContext(1);
        zmq::socket_t delayedSocket(delayedContext, zmq::socket_type::rep);
        delayedSocket.set(zmq::sockopt::linger, 0);
        delayedSocket.set(zmq::sockopt::rcvtimeo, 25);
        delayedSocket.bind(delayedEndpoint);
        delayedServerBound.set_value();

        RealTimeClock delayedClock;
        Network::ServerConfig delayedConfig = networkConfig;
        delayedConfig.inactivityTimeoutTics = 3 * kNsPerSec;
        Network::NetworkServer delayedServer(delayedClock, delayedConfig);
        while (delayedServerRunning.load()) {
            std::vector<zmq::message_t> raw;
            const auto received = zmq::recv_multipart(delayedSocket, std::back_inserter(raw));
            if (!received.has_value()) {
                continue;
            }

            Network::Message message;
            for (const zmq::message_t& field : raw) {
                message.push_back(field.to_string());
            }
            sendMessage(delayedSocket, delayedServer.handle(message));
        }
    });
    delayedServerReady.wait();

    clientClock.advance(kNsPerSec - 1);
    delayedClient.poll();
    passed &= expect(delayedClient.state() == Network::ConnectionState::Connecting,
                     "client should remain connecting during retry backoff");
    clientClock.advance(1);

    const auto connectionDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    while (delayedClient.state() != Network::ConnectionState::Connected &&
           std::chrono::steady_clock::now() < connectionDeadline) {
        delayedClient.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    passed &= expect(delayedClient.state() == Network::ConnectionState::Connected,
                     "client should join after a delayed server starts");

    // Exercise the public client API as well as the raw protocol sockets above.
    auto waitUntil = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return condition();
    };
    delayedClient.submitPosition(std::numeric_limits<float>::quiet_NaN(), 10.0F);
    passed &= expect(delayedClient.error() == "position must be finite",
                     "client should reject non-finite local positions before sending");
    delayedClient.submitPosition(150.5F, 180.25F);
    delayedClient.submitPosition(999.0F, 999.0F);
    passed &= expect(waitUntil([&] {
        delayedClient.poll();
        float x = 0.0F;
        return containsPlayer(delayedClient.snapshot(), delayedClient.playerId(), &x) && x == 150.5F;
    }), "client should send positions and skip submissions while a reply is outstanding");

    Network::NetworkClient observerOne(delayedEndpoint);
    Network::NetworkClient observerTwo(delayedEndpoint);
    observerOne.start();
    observerTwo.start();
    passed &= expect(waitUntil([&] {
        observerOne.poll();
        observerTwo.poll();
        return observerOne.state() == Network::ConnectionState::Connected &&
               observerTwo.state() == Network::ConnectionState::Connected;
    }), "two more public clients should join the running server");
    passed &= expect(waitUntil([&] {
        delayedClient.poll();
        observerOne.poll();
        observerTwo.poll();
        delayedClient.submitPosition(150.5F, 180.25F);
        observerOne.submitPosition(300.0F, 40.0F);
        observerTwo.submitPosition(400.0F, 50.0F);
        float firstX = 0.0F;
        float secondX = 0.0F;
        return delayedClient.snapshot().players.size() == 3 &&
               observerOne.snapshot().players.size() == 3 && observerTwo.snapshot().players.size() == 3 &&
               containsPlayer(observerOne.snapshot(), delayedClient.playerId(), &firstX) && firstX == 150.5F &&
               containsPlayer(observerTwo.snapshot(), observerOne.playerId(), &secondX) && secondX == 300.0F;
    }), "all three public clients should receive one another's reported positions");

    delayedClient.leave();
    observerOne.leave();
    observerTwo.leave();
    delayedServerRunning.store(false);
    delayedServerThread.join();

    return passed ? 0 : 1;
}
