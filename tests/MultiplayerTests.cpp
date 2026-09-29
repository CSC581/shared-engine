#include "Multiplayer.hpp"
#include "../sandbox/NetworkDemoConfig.hpp"
#include "NetworkServerHost.hpp"
#include "SendPacer.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
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

template <typename Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds budget = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// The engine's own server host on an ephemeral loopback port, so both modes
// are exercised against the real thing rather than a mock. Dedicated is what
// network-server runs; Listen is what a peer launched with --host runs.
std::unique_ptr<Network::NetworkServerHost> startServer(Network::HostMode mode)
{
    Network::HostConfig hostConfig;
    hostConfig.mode = mode;
    hostConfig.bindEndpoint = "tcp://127.0.0.1:*";
    hostConfig.pollInterval = std::chrono::milliseconds(20);
    auto host = std::make_unique<Network::NetworkServerHost>(NetworkDemo::makeServerConfig(), hostConfig);
    host->start();
    return host;
}

const Multiplayer::Player* findPlayer(const Multiplayer::Session& session, Multiplayer::PlayerId id)
{
    for (const Multiplayer::Player& player : session.remotePlayers()) {
        if (player.id == id) {
            return &player;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The heart of it. This function is what a game's frame loop does, written
// once and run against both architectures: pump, publish, and read back where
// everybody is. It mentions no server, no peer, no socket — if both modes pass
// it, the abstraction is real rather than a pair of classes sharing a header.
// ---------------------------------------------------------------------------
bool exerciseAsAGameWould(Multiplayer::Session& mine, Multiplayer::Session& theirs,
                          const std::string& label, bool expectPlatforms)
{
    bool passed = true;

    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(300.0F, 400.0F);
                         return mine.state() == Multiplayer::State::Ready &&
                                theirs.state() == Multiplayer::State::Ready;
                     }),
                     label + ": both sessions should become ready");

    passed &= expect(mine.localPlayerId() != 0 && theirs.localPlayerId() != 0,
                     label + ": both players should have an id once ready");
    passed &= expect(mine.localPlayerId() != theirs.localPlayerId(),
                     label + ": two players in one session should have different ids");

    // Each must see the other, at the position that other one published.
    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(300.0F, 400.0F);
                         const Multiplayer::Player* seen = findPlayer(mine, theirs.localPlayerId());
                         return seen != nullptr && seen->x == 300.0F && seen->y == 400.0F;
                     }),
                     label + ": a player should see the other player's published position");

    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(300.0F, 400.0F);
                         const Multiplayer::Player* seen = findPlayer(theirs, mine.localPlayerId());
                         return seen != nullptr && seen->x == 100.0F && seen->y == 200.0F;
                     }),
                     label + ": the other player should see us too");

    // A game's own player data must survive the trip, over either
    // architecture, with nothing in between understanding a byte of it. This
    // string is in a format invented here and known nowhere else.
    const std::string theirData = "strokes=4|club=7iron|facing=-1.5";

    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(300.0F, 400.0F, theirData);
                         const Multiplayer::Player* seen = findPlayer(mine, theirs.localPlayerId());
                         return seen != nullptr && seen->data == theirData;
                     }),
                     label + ": a game's own player data should arrive byte for byte");

    // Including bytes a text protocol might be tempted to treat as special.
    // ZeroMQ frames are length-delimited, so there is nothing to escape.
    const std::string awkward = std::string("tabs\tnewlines\nquotes\"'and spaces");
    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(300.0F, 400.0F, awkward);
                         const Multiplayer::Player* seen = findPlayer(mine, theirs.localPlayerId());
                         return seen != nullptr && seen->data == awkward;
                     }),
                     label + ": player data should survive separators and whitespace");

    // A player that sends nothing extra must still work: attributes are
    // optional, and a game that never uses them should not have to care.
    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(300.0F, 400.0F);
                         const Multiplayer::Player* seen = findPlayer(mine, theirs.localPlayerId());
                         return seen != nullptr && seen->data.empty();
                     }),
                     label + ": a player sending no data of its own is still a player");

    // The name must arrive over BOTH architectures. It did not, once: the
    // client-server wire had no name field and this interface quietly returned
    // an empty string for it.
    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(300.0F, 400.0F);
                         const Multiplayer::Player* seen = findPlayer(mine, theirs.localPlayerId());
                         return seen != nullptr && !seen->name.empty();
                     }),
                     label + ": a player's name should reach the other player");

    // A moved player must be seen to move.
    passed &= expect(waitUntil([&] {
                         mine.update();
                         theirs.update();
                         mine.publishLocalPlayer(100.0F, 200.0F);
                         theirs.publishLocalPlayer(555.5F, 66.25F);
                         const Multiplayer::Player* seen = findPlayer(mine, theirs.localPlayerId());
                         return seen != nullptr && seen->x == 555.5F && seen->y == 66.25F;
                     }),
                     label + ": a player that moves should be seen to move");

    // remotePlayers() must never contain the local player, in either mode, so
    // a game can draw them all without filtering.
    passed &= expect(findPlayer(mine, mine.localPlayerId()) == nullptr,
                     label + ": remotePlayers should never include the local player");
    passed &= expect(findPlayer(theirs, theirs.localPlayerId()) == nullptr,
                     label + ": remotePlayers should never include the local player");
    passed &= expect(mine.remotePlayers().size() == 1 && theirs.remotePlayers().size() == 1,
                     label + ": a two-player session should show exactly one other player");

    if (expectPlatforms) {
        passed &= expect(waitUntil([&] {
                             mine.update();
                             theirs.update();
                             mine.publishLocalPlayer(100.0F, 200.0F);
                             theirs.publishLocalPlayer(555.5F, 66.25F);
                             return mine.platforms().size() == 2 && theirs.platforms().size() == 2;
                         }),
                         label + ": both players should receive the shared world objects");

        // Shared objects must be shared: same ids, and moving.
        passed &= expect(mine.platforms()[0].id == theirs.platforms()[0].id &&
                             mine.platforms()[0].width > 0.0F,
                         label + ": shared objects should be the same objects for both players");
    }

    return passed;
}

bool testClientServer()
{
    const auto server = startServer(Network::HostMode::Dedicated);
    const std::string endpoint = server->boundEndpoint();
    RealTimeClock clock;

    Multiplayer::Config config;
    config.mode = Multiplayer::Mode::ClientServer;
    config.serverEndpoint = endpoint;
    config.playerName = "one";

    // Neither session is told anything about the other; the server introduces
    // them. Note that no id is configured — the server assigns both.
    std::unique_ptr<Multiplayer::Session> one = Multiplayer::Session::open(config, clock);
    config.playerName = "two";
    std::unique_ptr<Multiplayer::Session> two = Multiplayer::Session::open(config, clock);

    bool passed = expect(one->mode() == Multiplayer::Mode::ClientServer, "mode should be reported back");
    passed &= exerciseAsAGameWould(*one, *two, "client-server", true);
    passed &= expect(one->authorityState() == Multiplayer::AuthorityState::Ready,
                     "client-server mode should report its server as ready");
    return passed;
}

bool testPeerToPeer()
{
    const auto server = startServer(Network::HostMode::Listen);
    const std::string endpoint = server->boundEndpoint();
    RealTimeClock clock;

    Multiplayer::Config first;
    first.mode = Multiplayer::Mode::PeerToPeer;
    first.serverEndpoint = endpoint;
    first.peerId = 1;
    first.playerName = "one";
    first.basePort = 57410;

    Multiplayer::Config second = first;
    second.peerId = 2;
    second.playerName = "two";
    second.basePort = 57412;
    // One address is all a peer needs; the mesh introduces the rest.
    second.bootstrapPeers = {"tcp://127.0.0.1:57410"};

    std::unique_ptr<Multiplayer::Session> one = Multiplayer::Session::open(first, clock);
    std::unique_ptr<Multiplayer::Session> two = Multiplayer::Session::open(second, clock);

    bool passed = expect(one->mode() == Multiplayer::Mode::PeerToPeer, "mode should be reported back");
    passed &= expect(one->state() != Multiplayer::State::Failed,
                     "a peer session should start: " + one->status());
    // The same checks as client-server, against a completely different
    // architecture, with no change to the checks themselves.
    passed &= exerciseAsAGameWould(*one, *two, "peer-to-peer", true);
    passed &= expect(one->authorityState() == Multiplayer::AuthorityState::Ready,
                     "hybrid peer-to-peer mode should report its platform authority as ready");

    passed &= expect(server->playerCount() == 0,
                     "hybrid peers must not join the authority as server players");

    Multiplayer::Config regular;
    regular.mode = Multiplayer::Mode::ClientServer;
    regular.serverEndpoint = endpoint;
    regular.playerName = "regular";
    std::unique_ptr<Multiplayer::Session> clientServer = Multiplayer::Session::open(regular, clock);
    passed &= expect(waitUntil([&] { clientServer->update(); return clientServer->state() == Multiplayer::State::Ready; }),
                     "normal client should still join the authority");
    passed &= expect(clientServer->remotePlayers().empty() && server->playerCount() == 1,
                     "normal client must not see hybrid peers as duplicate server players");
    return passed;
}

// Peers with no server process anywhere: the case the client-server mode
// simply cannot express.
bool testPeerToPeerWithNoServerAtAll()
{
    RealTimeClock clock;

    Multiplayer::Config first;
    first.mode = Multiplayer::Mode::PeerToPeer;
    first.serverEndpoint = {};  // no authority, no server, no process to start
    first.peerId = 1;
    first.playerName = "one";
    first.basePort = 57420;

    Multiplayer::Config second = first;
    second.peerId = 2;
    second.playerName = "two";
    second.basePort = 57422;
    second.bootstrapPeers = {"tcp://127.0.0.1:57420"};

    std::unique_ptr<Multiplayer::Session> one = Multiplayer::Session::open(first, clock);
    std::unique_ptr<Multiplayer::Session> two = Multiplayer::Session::open(second, clock);

    bool passed = exerciseAsAGameWould(*one, *two, "peer-to-peer (serverless)", false);
    passed &= expect(one->platforms().empty(),
                     "a session with no authority should report no shared objects");
    passed &= expect(one->authorityState() == Multiplayer::AuthorityState::NotConfigured,
                     "a serverless peer session should say that no authority is configured");
    return passed;
}

// A game has to be able to draw a sensible screen before anything connects,
// and both modes must agree on what that looks like.
bool testBothModesBehaveBeforeConnecting()
{
    RealTimeClock clock;
    bool passed = true;

    Multiplayer::Config missing;
    missing.mode = Multiplayer::Mode::ClientServer;
    // Nothing is listening here.
    missing.serverEndpoint = "tcp://127.0.0.1:57499";
    std::unique_ptr<Multiplayer::Session> client = Multiplayer::Session::open(missing, clock);
    client->update();

    passed &= expect(client->state() != Multiplayer::State::Ready,
                     "a client with no server should not claim to be ready");
    passed &= expect(client->localPlayerId() == 0, "an unconnected client should have no id");
    passed &= expect(client->remotePlayers().empty() && client->platforms().empty(),
                     "an unconnected client should report an empty world");
    passed &= expect(!client->status().empty(), "an unconnected client should explain itself");
    // Publishing before connecting must be a no-op rather than a crash: a game
    // calls this every frame from the very first one.
    client->publishLocalPlayer(1.0F, 2.0F);
    client->update();

    // A port that is already taken is the peer-mode equivalent of a dead
    // server, and must be reported rather than thrown.
    Multiplayer::Config peer;
    peer.mode = Multiplayer::Mode::PeerToPeer;
    peer.serverEndpoint = {};
    peer.peerId = 1;
    peer.basePort = 57430;
    std::unique_ptr<Multiplayer::Session> held = Multiplayer::Session::open(peer, clock);
    passed &= expect(held->state() == Multiplayer::State::Ready,
                     "the first peer should hold its port: " + held->status());

    Multiplayer::Config collision = peer;
    collision.peerId = 2;
    std::unique_ptr<Multiplayer::Session> clash = Multiplayer::Session::open(collision, clock);
    passed &= expect(clash->state() == Multiplayer::State::Failed,
                     "a peer whose port is taken should report failure, not throw");
    passed &= expect(!clash->status().empty(), "a failed peer should say why");
    clash->update();
    clash->publishLocalPlayer(1.0F, 2.0F);
    passed &= expect(clash->remotePlayers().empty(),
                     "a failed peer should stay usable enough to draw an empty screen");

    Multiplayer::Config waiting = peer;
    waiting.peerId = 3;
    waiting.basePort = 57432;
    waiting.serverEndpoint = "tcp://127.0.0.1:57499";
    std::unique_ptr<Multiplayer::Session> hybrid = Multiplayer::Session::open(waiting, clock);
    hybrid->update();
    passed &= expect(hybrid->state() == Multiplayer::State::Ready,
                     "the peer mesh should still be usable while its authority connects");
    passed &= expect(hybrid->authorityState() != Multiplayer::AuthorityState::Ready,
                     "authorityState should reveal that shared world objects are not ready");

    return passed;
}

bool testPeerReportsTerminalAuthorityFailure()
{
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, 2000);
    socket.bind("tcp://127.0.0.1:*");
    const std::string endpoint = socket.get(zmq::sockopt::last_endpoint);

    std::thread authority([moved = std::move(socket)]() mutable {
        bool received = false;
        try {
            Net::receive(moved, received);
            if (received) {
                Net::send(moved, {std::to_string(Network::protocolVersion + 1),
                                  "WORLD_STATE", "0", "0"});
            }
        } catch (const zmq::error_t&) {
        }
    });

    RealTimeClock clock;
    Multiplayer::Config config;
    config.mode = Multiplayer::Mode::PeerToPeer;
    config.serverEndpoint = endpoint;
    config.peerId = 4;
    config.basePort = 57434;
    std::unique_ptr<Multiplayer::Session> session = Multiplayer::Session::open(config, clock);

    const bool failureReported = waitUntil([&] {
        session->update();
        return session->authorityState() == Multiplayer::AuthorityState::Failed;
    });
    authority.join();

    const std::string status = session->status();
    bool passed = expect(failureReported,
                         "a terminal authority error should be reported as failed");
    passed &= expect(session->state() == Multiplayer::State::Ready,
                     "an authority failure must not disable the usable peer mesh");
    passed &= expect(status.find("Protocol version mismatch") != std::string::npos,
                     "peer status should expose the authority's actual failure: " + status);
    passed &= expect(status.find("waiting for world objects") == std::string::npos,
                     "a failed authority must not still be described as waiting");
    return passed;
}

// Losing the authority must not delete the level. Before this, the client
// dropped its whole snapshot on a disconnect and the platforms went with it —
// so a server hiccup opened a hole in the floor under every player.
bool testWorldSurvivesLosingTheAuthority()
{
    bool passed = true;
    RealTimeClock clock;

    auto server = startServer(Network::HostMode::Dedicated);
    const std::string endpoint = server->boundEndpoint();

    Multiplayer::Config config;
    config.mode = Multiplayer::Mode::ClientServer;
    config.serverEndpoint = endpoint;
    std::unique_ptr<Multiplayer::Session> session = Multiplayer::Session::open(config, clock);

    passed &= expect(waitUntil([&] {
                         session->update();
                         session->publishLocalPlayer(10.0F, 20.0F);
                         return session->platforms().size() == 2;
                     }),
                     "the session should receive the world from the authority");

    const std::vector<Multiplayer::Platform> before = session->platforms();

    // The authority goes away entirely.
    server.reset();

    // Well past the point where the client has given up and started retrying.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < until) {
        session->update();
        session->publishLocalPlayer(10.0F, 20.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    passed &= expect(session->state() != Multiplayer::State::Ready,
                     "a session whose authority died should stop claiming to be ready");
    passed &= expect(session->platforms().size() == before.size(),
                     "the world should survive losing the authority, not vanish");
    passed &= expect(!session->platforms().empty() &&
                         session->platforms()[0].id == before[0].id &&
                         session->platforms()[0].width == before[0].width,
                     "the surviving world should be the one last known, unchanged");

    // Players are the opposite case and must NOT survive: a player whose owner
    // is unreachable is a ghost, and drawing it would be a lie.
    passed &= expect(session->remotePlayers().empty(),
                     "remote players should not survive losing the authority");

    return passed;
}


// ---------------------------------------------------------------------------
// Send pacing (§4): a client's send rate must follow its game speed, so a
// server really sees one client at half rate and another at double. Driven by
// hand-stepped clocks, like the engine's loop at 60 fps, so it needs no sleep
// and no sockets.
// ---------------------------------------------------------------------------
constexpr std::int64_t frameNs = kNsPerSec / 60;
constexpr std::int64_t sendIntervalUs = kGameTicsPerSecond / 30;
constexpr std::int64_t heartbeatNs = 250 * kNsPerMs;

// Sends in one real second of 60 frames, with the game timeline set up by
// `configure` — the engine's own game time: microseconds on a real clock.
template <typename Configure>
int sendsInOneSecond(std::int64_t interval, Configure configure)
{
    ManualClock real;
    Timeline gameTime(real, kNsPerUs);
    configure(gameTime);
    Multiplayer::SendPacer pacer(&gameTime, interval, real, heartbeatNs);

    // Warm up past the first send, so only the steady rate is counted.
    pacer.shouldSend();
    int sends = 0;
    for (int frame = 0; frame < 60; ++frame) {
        real.advance(frameNs);
        sends += pacer.shouldSend() ? 1 : 0;
    }
    return sends;
}

bool near(int actual, int expected) { return actual >= expected - 1 && actual <= expected + 1; }

bool testSendRateFollowsGameSpeed()
{
    bool passed = true;

    const int normal = sendsInOneSecond(sendIntervalUs, [](Timeline&) {});
    const int half = sendsInOneSecond(sendIntervalUs, [](Timeline& t) { t.setScale(0.5); });
    const int twice = sendsInOneSecond(sendIntervalUs, [](Timeline& t) { t.setScale(2.0); });
    const int paused = sendsInOneSecond(sendIntervalUs, [](Timeline& t) { t.pause(); });
    const int unpaced = sendsInOneSecond(0, [](Timeline& t) { t.setScale(0.5); });

    passed &= expect(near(normal, 30), "1x should send 30/s, sent " + std::to_string(normal));
    passed &= expect(near(half, 15), "0.5x should halve the rate, sent " + std::to_string(half));
    passed &= expect(near(twice, 60), "2x should double the rate, sent " + std::to_string(twice));
    passed &= expect(near(paused, 4),
                     "paused should send only the 4/s heartbeat, sent " + std::to_string(paused));
    passed &= expect(unpaced == 60,
                     "without pacing every publish should send, sent " + std::to_string(unpaced));
    return passed;
}

// A stall must not be paid back as a burst: after one long frame the next send
// is simply on time again.
bool testSendPacingDoesNotBurstAfterAStall()
{
    ManualClock real;
    Timeline gameTime(real, kNsPerUs);
    Multiplayer::SendPacer pacer(&gameTime, sendIntervalUs, real, heartbeatNs);
    pacer.shouldSend();

    real.advance(kNsPerSec / 5); // a 200 ms hitch: six intervals late
    bool passed = expect(pacer.shouldSend(), "the frame after a stall should send");

    real.advance(frameNs);
    passed &= expect(!pacer.shouldSend(), "a stall should not cause catch-up sends");
    return passed;
}

bool testPacingWithoutAGameClockIsRejected()
{
    Multiplayer::Config config;
    config.sendIntervalGameTics = sendIntervalUs;
    RealTimeClock real;
    try {
        Multiplayer::Session::open(config, real);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return expect(false, "pacing with no game clock should be rejected as a config error");
}

} // namespace

int main()
{
    bool passed = true;

    passed &= testClientServer();
    passed &= testPeerToPeer();
    passed &= testPeerToPeerWithNoServerAtAll();
    passed &= testBothModesBehaveBeforeConnecting();
    passed &= testPeerReportsTerminalAuthorityFailure();
    passed &= testWorldSurvivesLosingTheAuthority();
    passed &= testSendRateFollowsGameSpeed();
    passed &= testSendPacingDoesNotBurstAfterAStall();
    passed &= testPacingWithoutAGameClockIsRejected();

    if (passed) {
        std::cout << "multiplayer-tests: all checks passed\n";
    }
    return passed ? 0 : 1;
}
