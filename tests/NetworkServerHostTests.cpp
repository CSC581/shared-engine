#include "NetworkClient.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkServerHost.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Everything here runs the real NetworkServerHost on loopback: no hand-built
// REP loops standing in for it.
namespace {

using Clock = std::chrono::steady_clock;

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

Network::ServerConfig testServerConfig()
{
    Network::ServerConfig config;
    config.spawnPoints = {{10.0F, 20.0F}, {30.0F, 20.0F}, {50.0F, 20.0F}};
    config.platforms = {{7, 0.0F, 100.0F, 200.0F, 100.0F, 80.0F, 96.0F, 20.0F}};
    return config;
}

Network::HostConfig testHostConfig(Network::HostMode mode)
{
    Network::HostConfig config;
    config.mode = mode;
    config.bindEndpoint = "tcp://127.0.0.1:*";
    config.advertiseHost = "127.0.0.1";
    // Short polls keep reaping and shutdown quick under test.
    config.pollInterval = std::chrono::milliseconds(20);
    return config;
}

Network::SessionToken token(char digit)
{
    return Network::SessionToken(32, digit);
}

// One request on a fresh REQ socket. A fresh socket per call keeps a failed
// exchange from leaving a REQ stuck in its send/receive lockstep.
bool call(zmq::context_t& context, const std::string& endpoint, const Network::Message& message,
          Network::Reply& reply)
{
    zmq::socket_t socket(context, zmq::socket_type::req);
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, 2000);
    socket.set(zmq::sockopt::sndtimeo, 2000);
    socket.connect(endpoint);
    if (!Net::send(socket, message)) {
        return false;
    }
    bool received = false;
    const Network::Message raw = Net::receive(socket, received);
    std::string error;
    return received && Network::decodeReply(raw, reply, error);
}

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit)
{
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

bool hasPlayerAt(const Network::WorldSnapshot& snapshot, Network::PlayerId id, float x)
{
    return std::any_of(snapshot.players.begin(), snapshot.players.end(),
                       [&](const Network::PlayerState& player) { return player.id == id && player.x == x; });
}

bool rejectsBadConfig()
{
    bool passed = true;
    const auto throwsInvalid = [](Network::HostConfig config) {
        try {
            Network::NetworkServerHost host(testServerConfig(), std::move(config));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    Network::HostConfig emptyBind = testHostConfig(Network::HostMode::Dedicated);
    emptyBind.bindEndpoint.clear();
    passed &= expect(throwsInvalid(emptyBind), "empty bind endpoint should be rejected");

    Network::HostConfig noAdvertise = testHostConfig(Network::HostMode::Dedicated);
    noAdvertise.advertiseHost.clear();
    passed &= expect(throwsInvalid(noAdvertise), "dedicated host needs an advertise host");

    Network::HostConfig zeroPoll = testHostConfig(Network::HostMode::Dedicated);
    zeroPoll.pollInterval = std::chrono::milliseconds(0);
    passed &= expect(throwsInvalid(zeroPoll), "zero poll interval should be rejected");

    Network::HostConfig listenNoAdvertise = testHostConfig(Network::HostMode::Listen);
    listenNoAdvertise.advertiseHost.clear();
    passed &= expect(!throwsInvalid(listenNoAdvertise), "listen host does not advertise workers");
    return passed;
}

// Section 4: each JOIN is handed its own worker socket, and what one client
// reports on its worker reaches the others.
bool joinsGetPrivateWorkers()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    host.start();
    const std::string handshake = host.boundEndpoint();
    zmq::context_t context(1);

    bool passed = true;
    std::vector<Network::Reply> welcomes(3);
    std::set<std::string> endpoints;
    for (int i = 0; i < 3; ++i) {
        passed &= expect(call(context, handshake, Network::encodeJoin(token(static_cast<char>('1' + i))), welcomes[i]) &&
                             welcomes[i].type == Network::ReplyType::Welcome,
                         "JOIN should be welcomed");
        endpoints.insert(welcomes[i].sessionEndpoint);
    }
    if (!passed) {
        return false;
    }

    passed &= expect(endpoints.size() == 3 && endpoints.count(handshake) == 0 && endpoints.count("") == 0,
                     "each client should get a distinct worker endpoint, not the handshake");
    passed &= expect(host.playerCount() == 3 && host.activeWorkers() == 3,
                     "three players should have three workers");

    Network::Reply moved;
    passed &= expect(call(context, welcomes[0].sessionEndpoint,
                          Network::encodePosition(welcomes[0].playerId, token('1'), {123.0F, 45.0F, 1, {}}), moved) &&
                         moved.type == Network::ReplyType::Snapshot,
                     "POSITION on a worker should return a snapshot");

    Network::Reply seen;
    passed &= expect(call(context, welcomes[1].sessionEndpoint,
                          Network::encodePosition(welcomes[1].playerId, token('2'), {5.0F, 5.0F, 1, {}}), seen) &&
                         hasPlayerAt(seen.snapshot, welcomes[0].playerId, 123.0F),
                     "another client's snapshot should show the first client's move");
    return passed;
}

// Section 2 end to end: three real NetworkClients see each other through the
// real host, having moved from the handshake to their own workers.
bool threeNetworkClientsSeeEachOther()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    host.start();
    RealTimeClock clock;

    std::vector<std::unique_ptr<Network::NetworkClient>> clients;
    for (int i = 0; i < 3; ++i) {
        clients.push_back(std::make_unique<Network::NetworkClient>(clock, host.boundEndpoint()));
        clients.back()->start();
    }

    const auto pump = [&](float step) {
        for (std::size_t i = 0; i < clients.size(); ++i) {
            clients[i]->poll();
            clients[i]->submitPosition(100.0F * static_cast<float>(i + 1) + step, 0.0F);
        }
    };

    float step = 0.0F;
    const bool everyoneSeesEveryone = waitFor(
        [&] {
            pump(step += 1.0F);
            return std::all_of(clients.begin(), clients.end(), [](const auto& client) {
                return client->state() == Network::ConnectionState::Connected &&
                       client->snapshot().players.size() == 3;
            });
        },
        std::chrono::milliseconds(3000));

    bool passed = expect(everyoneSeesEveryone, "three clients should each see all three players");
    passed &= expect(host.activeWorkers() == 3, "each client should be on its own worker");
    for (auto& client : clients) {
        client->leave();
    }
    return passed;
}

// Workers answer concurrently: two clients hammering their own workers while a
// third sits idle all get prompt replies.
bool concurrentClientsAreServedOnTheirOwnWorkers()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    host.start();
    zmq::context_t context(1);

    std::vector<Network::Reply> welcomes(3);
    for (int i = 0; i < 3; ++i) {
        if (!expect(call(context, host.boundEndpoint(), Network::encodeJoin(token(static_cast<char>('a' + i))),
                         welcomes[i]),
                    "JOIN should be welcomed")) {
            return false;
        }
    }

    std::atomic<int> failures{0};
    std::atomic<long long> slowestMs{0};
    const auto hammer = [&](int index) {
        for (std::uint64_t sequence = 1; sequence <= 50; ++sequence) {
            const auto begin = Clock::now();
            Network::Reply reply;
            const bool ok = call(context, welcomes[index].sessionEndpoint,
                                 Network::encodePosition(welcomes[index].playerId,
                                                         token(static_cast<char>('a' + index)),
                                                         {1.0F, 1.0F, sequence, {}}),
                                 reply) &&
                            reply.type == Network::ReplyType::Snapshot;
            const long long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
            if (!ok) {
                ++failures;
            }
            long long seen = slowestMs.load();
            while (ms > seen && !slowestMs.compare_exchange_weak(seen, ms)) {
            }
        }
    };

    std::thread first(hammer, 1);
    std::thread second(hammer, 2);
    first.join();
    second.join();

    bool passed = expect(failures.load() == 0, "every round-trip on a worker should succeed");
    passed &= expect(slowestMs.load() < 500,
                     "round-trips should stay prompt, slowest was " + std::to_string(slowestMs.load()) + "ms");
    return passed;
}

bool getWorldOnHandshakeDoesNotJoin()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    host.start();
    zmq::context_t context(1);

    Network::Reply reply;
    bool passed = expect(call(context, host.boundEndpoint(), Network::encodeGetWorld(), reply) &&
                             reply.type == Network::ReplyType::WorldState &&
                             reply.worldState.platforms.size() == 1,
                         "GET_WORLD on the handshake should return the platforms");
    passed &= expect(host.playerCount() == 0 && host.activeWorkers() == 0,
                     "GET_WORLD should not add a player or a worker");
    return passed;
}

bool rejoinReusesWorker()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    host.start();
    zmq::context_t context(1);

    Network::Reply first;
    Network::Reply second;
    bool passed = expect(call(context, host.boundEndpoint(), Network::encodeJoin(token('5')), first) &&
                             call(context, host.boundEndpoint(), Network::encodeJoin(token('5')), second),
                         "both JOINs should be answered");
    passed &= expect(first.type == Network::ReplyType::Welcome && second.type == Network::ReplyType::Welcome &&
                         first.sessionEndpoint == second.sessionEndpoint && first.playerId == second.playerId,
                     "rejoin with the same token should return the same worker and player");
    passed &= expect(host.activeWorkers() == 1 && host.playerCount() == 1,
                     "rejoin should not start a second worker");
    return passed;
}

// A departed client's worker is joined and dropped while the host keeps
// running, rather than piling up until shutdown.
bool leaveReapsWorker()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    host.start();
    zmq::context_t context(1);

    Network::Reply welcome;
    if (!expect(call(context, host.boundEndpoint(), Network::encodeJoin(token('6')), welcome),
                "JOIN should be welcomed")) {
        return false;
    }

    Network::Reply rejected;
    bool passed = expect(call(context, welcome.sessionEndpoint, Network::encodeLeave(welcome.playerId, token('7')),
                              rejected) &&
                             rejected.type == Network::ReplyType::Error,
                         "LEAVE with someone else's token should be refused");
    passed &= expect(host.activeWorkers() == 1, "a refused LEAVE should keep the worker");

    Network::Reply goodbye;
    passed &= expect(call(context, welcome.sessionEndpoint, Network::encodeLeave(welcome.playerId, token('6')),
                          goodbye) &&
                         goodbye.type == Network::ReplyType::Goodbye,
                     "LEAVE should be answered with GOODBYE");
    passed &= expect(waitFor([&] { return host.activeWorkers() == 0; }, std::chrono::milliseconds(2000)),
                     "the departed client's worker should be reaped");
    passed &= expect(host.playerCount() == 0, "the player should be gone");

    Network::Reply again;
    passed &= expect(call(context, host.boundEndpoint(), Network::encodeJoin(token('6')), again) &&
                         again.type == Network::ReplyType::Welcome &&
                         again.sessionEndpoint != welcome.sessionEndpoint,
                     "joining again after LEAVE should get a new worker");
    return passed;
}

bool handshakeRejectsOtherRequests()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    host.start();
    zmq::context_t context(1);

    Network::Reply position;
    bool passed = expect(call(context, host.boundEndpoint(),
                              Network::encodePosition(1, token('1'), {1.0F, 1.0F, 1, {}}), position) &&
                             position.type == Network::ReplyType::Error &&
                             position.error.find("JOIN only") != std::string::npos,
                         "POSITION on the handshake should be refused");

    Network::Reply garbage;
    passed &= expect(call(context, host.boundEndpoint(), {"not", "a", "request"}, garbage) &&
                         garbage.type == Network::ReplyType::Error,
                     "an undecodable request should get an error, not silence");
    passed &= expect(host.playerCount() == 0 && host.activeWorkers() == 0,
                     "refused requests should not create players or workers");
    return passed;
}

bool takenAddressThrows()
{
    Network::NetworkServerHost first(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
    first.start();

    Network::HostConfig same = testHostConfig(Network::HostMode::Listen);
    same.bindEndpoint = first.boundEndpoint();
    Network::NetworkServerHost second(testServerConfig(), same);
    bool threw = false;
    try {
        second.start();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    bool passed = expect(threw, "binding a taken address should throw std::runtime_error");
    passed &= expect(!second.running(), "a host that failed to bind should not be running");

    bool restartRefused = false;
    try {
        first.start();
    } catch (const std::logic_error&) {
        restartRefused = true;
    }
    passed &= expect(restartRefused, "starting a host twice should throw std::logic_error");
    return passed;
}

bool stopIsPromptAndIdempotent()
{
    bool passed = true;
    zmq::context_t context(1);

    const auto begin = Clock::now();
    {
        Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Dedicated));
        host.stop(); // before start: nothing to do
        host.start();
        Network::Reply a;
        Network::Reply b;
        passed &= expect(call(context, host.boundEndpoint(), Network::encodeJoin(token('8')), a) &&
                             call(context, host.boundEndpoint(), Network::encodeJoin(token('9')), b),
                         "JOINs should be welcomed");
        host.stop();
        passed &= expect(!host.running() && host.activeWorkers() == 0, "stop should join every worker");
        host.stop();
        // Destructor runs a third stop with live-client state still around.
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin);
    passed &= expect(elapsed.count() < 3000, "shutdown should not hang, took " + std::to_string(elapsed.count()) + "ms");
    return passed;
}

// Listen mode: one socket, every request answered on it, no workers.
bool listenModeServesEverythingOnOneSocket()
{
    Network::NetworkServerHost host(testServerConfig(), testHostConfig(Network::HostMode::Listen));
    host.start();
    zmq::context_t context(1);
    const std::string endpoint = host.boundEndpoint();

    Network::Reply world;
    bool passed = expect(call(context, endpoint, Network::encodeGetWorld(), world) &&
                             world.type == Network::ReplyType::WorldState && world.worldState.platforms.size() == 1,
                         "listen host should answer GET_WORLD");

    Network::Reply welcome;
    passed &= expect(call(context, endpoint, Network::encodeJoin(token('c')), welcome) &&
                         welcome.type == Network::ReplyType::Welcome && welcome.sessionEndpoint.empty(),
                     "listen host should welcome without handing out a worker");

    Network::Reply snapshot;
    passed &= expect(call(context, endpoint, Network::encodePosition(welcome.playerId, token('c'), {9.0F, 9.0F, 1, {}}),
                          snapshot) &&
                         hasPlayerAt(snapshot.snapshot, welcome.playerId, 9.0F),
                     "listen host should accept POSITION on the same socket");
    passed &= expect(host.activeWorkers() == 0, "listen host should never start workers");

    // Server-owned platforms keep moving on the host's tick thread. One request
    // advances them at most once, so several revisions across a quiet period
    // can only have come from the ticks.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Network::Reply later;
    passed &= expect(call(context, endpoint, Network::encodeGetWorld(), later) &&
                         later.worldState.worldRevision >= world.worldState.worldRevision + 3,
                     "platforms should advance on the tick thread with no requests driving them");
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    try {
        passed &= rejectsBadConfig();
        passed &= joinsGetPrivateWorkers();
        passed &= threeNetworkClientsSeeEachOther();
        passed &= concurrentClientsAreServedOnTheirOwnWorkers();
        passed &= getWorldOnHandshakeDoesNotJoin();
        passed &= rejoinReusesWorker();
        passed &= leaveReapsWorker();
        passed &= handshakeRejectsOtherRequests();
        passed &= takenAddressThrows();
        passed &= stopIsPromptAndIdempotent();
        passed &= listenModeServesEverythingOnOneSocket();
    } catch (const std::exception& exception) {
        std::cerr << "Test failed with exception: " << exception.what() << '\n';
        passed = false;
    }

    if (!passed) {
        return 1;
    }
    std::cout << "Network server host tests passed\n";
    return 0;
}
