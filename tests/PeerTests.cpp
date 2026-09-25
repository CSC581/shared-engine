#include "PeerProtocol.hpp"
#include "PeerSession.hpp"
#include "WireFormat.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
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

// Spins a predicate until it holds or the budget runs out. Peer discovery is
// several round trips across three processes' worth of threads, so a test that
// slept a fixed amount would be either slow or flaky; this is neither.
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

Peer::PeerAddress makeAddress(Peer::PeerId id, const std::string& name)
{
    Peer::PeerAddress address;
    address.id = id;
    address.name = name;
    address.pubEndpoint = "tcp://127.0.0.1:7100";
    address.greetEndpoint = "tcp://127.0.0.1:7101";
    return address;
}

bool testProtocolRoundTrips()
{
    bool passed = true;

    const Peer::PeerAddress address = makeAddress(7, "alice");
    Peer::Envelope envelope;
    std::string error;

    passed &= expect(Peer::decode(Peer::encodeHello(address), envelope, error),
                     "HELLO should decode: " + error);
    passed &= expect(envelope.type == Peer::MessageType::Hello && envelope.senderId == 7 &&
                         envelope.address.name == "alice" &&
                         envelope.address.pubEndpoint == address.pubEndpoint &&
                         envelope.address.greetEndpoint == address.greetEndpoint,
                     "HELLO should round-trip the sender's address");

    const std::vector<Peer::PeerAddress> roster{makeAddress(1, "a"), makeAddress(2, "b")};
    passed &= expect(Peer::decode(Peer::encodeRoster(1, roster), envelope, error),
                     "ROSTER should decode: " + error);
    passed &= expect(envelope.type == Peer::MessageType::Roster && envelope.roster.size() == 2 &&
                         envelope.roster[1].id == 2 && envelope.roster[1].name == "b",
                     "ROSTER should round-trip every listed peer");

    passed &= expect(Peer::decode(Peer::encodeRoster(1, {}), envelope, error) &&
                         envelope.roster.empty(),
                     "an empty ROSTER should decode: " + error);

    Peer::PeerState state;
    state.id = 3;
    state.name = "carol";
    state.sequence = 91;
    state.x = 120.5F;
    state.y = -40.25F;
    state.velocityX = 220.0F;
    state.velocityY = -0.5F;
    state.health = 73;
    state.ready = true;
    passed &= expect(Peer::decode(Peer::encodeState(state), envelope, error),
                     "STATE should decode: " + error);
    passed &= expect(envelope.type == Peer::MessageType::State && envelope.state.id == 3 &&
                         envelope.state.name == "carol" && envelope.state.sequence == 91 &&
                         envelope.state.x == 120.5F && envelope.state.y == -40.25F &&
                         envelope.state.velocityX == 220.0F && envelope.state.velocityY == -0.5F &&
                         envelope.state.health == 73 && envelope.state.ready,
                     "STATE should round-trip exactly, floats included");

    passed &= expect(Peer::decode(Peer::encodePing(5), envelope, error) &&
                         envelope.type == Peer::MessageType::Ping && envelope.senderId == 5,
                     "PING should decode: " + error);

    passed &= expect(Peer::decode(Peer::encodeLeave(6), envelope, error) &&
                         envelope.type == Peer::MessageType::Leave && envelope.senderId == 6,
                     "LEAVE should decode: " + error);

    passed &= expect(Peer::decode(Peer::encodeError("no"), envelope, error) &&
                         envelope.type == Peer::MessageType::Error && envelope.error == "no",
                     "ERROR should decode: " + error);

    return passed;
}

bool testProtocolRejectsBadMessages()
{
    bool passed = true;
    Peer::Envelope envelope;
    std::string error;

    passed &= expect(!Peer::decode({}, envelope, error), "an empty message should be rejected");
    passed &= expect(!Peer::decode({"1", "LEAVE"}, envelope, error),
                     "a message with no sender should be rejected");
    passed &= expect(!Peer::decode({"99", "LEAVE", "1"}, envelope, error) &&
                         error == "unsupported peer protocol version",
                     "a future protocol version should be named as the problem");
    passed &= expect(!Peer::decode({"3", "PING", "1", "extra"}, envelope, error),
                     "a PING with extra fields should be rejected");
    passed &= expect(!Peer::decode({"1", "SHOUT", "1"}, envelope, error),
                     "an unknown command should be rejected");
    passed &= expect(!Peer::decode({"1", "LEAVE", "0"}, envelope, error),
                     "peer id 0 should be rejected");
    passed &= expect(!Peer::decode({"1", "LEAVE", "-3"}, envelope, error),
                     "a negative peer id should be rejected");
    passed &= expect(!Peer::decode({"1", "LEAVE", "4294967296"}, envelope, error),
                     "a peer id past 32 bits should be rejected");

    // A HELLO whose body claims a different peer than its own header is the
    // shape an impersonation attempt would take.
    Peer::Message hello = Peer::encodeHello(makeAddress(7, "alice"));
    hello[3] = "8";
    passed &= expect(!Peer::decode(hello, envelope, error),
                     "a HELLO whose body and header disagree should be rejected");

    Peer::Message badEndpoint = Peer::encodeHello(makeAddress(7, "alice"));
    badEndpoint[5] = "http://example.com";
    passed &= expect(!Peer::decode(badEndpoint, envelope, error),
                     "an endpoint on a transport we do not dial should be rejected");

    Peer::PeerState state;
    state.id = 1;
    state.name = "a";
    state.sequence = 1;
    Peer::Message badName = Peer::encodeState(state);
    badName[3] = std::string("bad\nname");
    passed &= expect(!Peer::decode(badName, envelope, error),
                     "a name with control characters should be rejected");

    Peer::Message zeroSequence = Peer::encodeState(state);
    zeroSequence[4] = "0";
    passed &= expect(!Peer::decode(zeroSequence, envelope, error),
                     "a STATE with sequence 0 should be rejected");

    Peer::Message shortRoster = Peer::encodeRoster(1, {makeAddress(1, "a")});
    shortRoster.pop_back();
    passed &= expect(!Peer::decode(shortRoster, envelope, error),
                     "a ROSTER whose count does not match its body should be rejected");

    // A game's own player data: carried verbatim, but still bounded, because
    // it arrives from another machine.
    Peer::PeerState withData;
    withData.id = 1;
    withData.name = "a";
    withData.sequence = 1;
    withData.data = std::string("strokes=4\tclub=7iron\nfacing=-1.5");
    passed &= expect(Peer::decode(Peer::encodeState(withData), envelope, error) &&
                         envelope.state.data == withData.data,
                     "STATE should round-trip a game's own data byte for byte: " + error);

    Peer::PeerState none;
    none.id = 1;
    none.name = "a";
    none.sequence = 1;
    passed &= expect(Peer::decode(Peer::encodeState(none), envelope, error) &&
                         envelope.state.data.empty(),
                     "a STATE with no game data should decode: " + error);

    Peer::PeerState tooBig = none;
    tooBig.data = std::string(Net::maxPlayerDataLength + 1, 'x');
    passed &= expect(!Peer::decode(Peer::encodeState(tooBig), envelope, error),
                     "player data past the size cap should be rejected");

    Peer::Message trailing = Peer::encodeState(withData);
    trailing.push_back("extra");
    passed &= expect(!Peer::decode(trailing, envelope, error),
                     "fields past the end of a STATE should be rejected");

    return passed;
}

// The real thing: three PeerSessions on real TCP sockets, in one process but
// sharing nothing — no pointers between them, only messages. Peers two and
// three are told about peer one and nothing else, so the mesh has to build
// itself out of one address.
bool testThreePeerMeshFormsFromOneAddress()
{
    bool passed = true;

    auto makeConfig = [](Peer::PeerId id, const std::string& name,
                         const std::vector<std::string>& bootstrap) {
        Peer::PeerSession::Config config;
        config.id = id;
        config.name = name;
        // Ephemeral ports, so the test never collides with a real session or
        // with another copy of itself.
        config.pubBind = "tcp://127.0.0.1:*";
        config.greetBind = "tcp://127.0.0.1:*";
        config.advertiseHost = "127.0.0.1";
        config.bootstrap = bootstrap;
        return config;
    };

    auto one = std::make_unique<Peer::PeerSession>(makeConfig(1, "one", {}));
    one->start();
    const std::string anchor = one->self().greetEndpoint;
    passed &= expect(!anchor.empty(), "a started peer should report its greet endpoint");
    passed &= expect(one->peerCount() == 1, "a lone peer knows only itself");

    // Peer three is bootstrapped off peer two, not peer one, so the roster has
    // to travel a hop for the mesh to close.
    auto two = std::make_unique<Peer::PeerSession>(makeConfig(2, "two", {anchor}));
    two->start();
    passed &= expect(waitUntil([&] { return one->peerCount() == 2 && two->peerCount() == 2; }),
                     "two peers should find each other from one address");

    auto three = std::make_unique<Peer::PeerSession>(makeConfig(3, "three", {two->self().greetEndpoint}));
    three->start();
    passed &= expect(waitUntil([&] {
                         return one->peerCount() == 3 && two->peerCount() == 3 &&
                                three->peerCount() == 3;
                     }),
                     "a third peer should reach the whole mesh through one member of it");

    passed &= expect(one->knows(3) && three->knows(1),
                     "peers introduced only indirectly should still know each other");

    const std::vector<Peer::PeerAddress> roster = three->roster();
    passed &= expect(roster.size() == 3 && roster[0].id == 1 && roster[1].id == 2 && roster[2].id == 3,
                     "a roster should be sorted by peer id");
    passed &= expect(roster[0].name == "one" && roster[2].name == "three",
                     "a roster should carry each peer's name");

    // State published by one peer must reach both others directly.
    Peer::PeerState state;
    state.id = 1;
    state.name = "one";
    state.sequence = 1;
    state.x = 321.5F;
    state.y = 654.25F;

    auto sawState = [](Peer::PeerSession& session, float& x) {
        for (const Peer::Envelope& envelope : session.drain()) {
            if (envelope.type == Peer::MessageType::State && envelope.state.id == 1) {
                x = envelope.state.x;
                return true;
            }
        }
        return false;
    };

    float atTwo = 0.0F;
    float atThree = 0.0F;
    bool twoSaw = false;
    bool threeSaw = false;
    passed &= expect(waitUntil([&] {
                         // Re-published each attempt: a PUB drops messages sent
                         // before a subscriber has finished connecting.
                         one->publishState(state);
                         twoSaw |= sawState(*two, atTwo);
                         threeSaw |= sawState(*three, atThree);
                         return twoSaw && threeSaw;
                     }),
                     "state published by one peer should reach every other peer directly");
    passed &= expect(atTwo == 321.5F && atThree == 321.5F,
                     "peer state should arrive unchanged at every peer");

    // Before testing a departure, make sure the traffic actually flows the
    // other way too. The checks above only prove that what peer one publishes
    // reaches the others; a subscription in the opposite direction is a
    // separate connection, and it is the one a LEAVE has to travel over.
    Peer::PeerState fromThree;
    fromThree.id = 3;
    fromThree.name = "three";
    fromThree.sequence = 1;
    fromThree.x = 77.5F;
    passed &= expect(waitUntil([&] {
                         three->publishState(fromThree);
                         for (const Peer::Envelope& envelope : one->drain()) {
                             if (envelope.type == Peer::MessageType::State && envelope.state.id == 3) {
                                 return true;
                             }
                         }
                         return false;
                     }),
                     "every peer should hear every other peer, in both directions");

    // A clean exit is announced rather than waited out. Peer two is kept
    // talking throughout: a peer that publishes nothing for long enough is
    // presumed gone by design, so a silent peer would drop out on its own and
    // this check would prove nothing about the departure of peer three.
    Peer::PeerState fromTwo;
    fromTwo.id = 2;
    fromTwo.name = "two";
    fromTwo.sequence = 1;

    three.reset();
    passed &= expect(waitUntil([&] {
                         fromTwo.sequence++;
                         two->publishState(fromTwo);
                         for (const Peer::Envelope& envelope : one->drain()) {
                             if (envelope.type == Peer::MessageType::Leave && envelope.senderId == 3) {
                                 return true;
                             }
                         }
                         return false;
                     }),
                     "a departing peer should announce LEAVE");
    passed &= expect(waitUntil([&] {
                         fromTwo.sequence++;
                         two->publishState(fromTwo);
                         return !one->knows(3);
                     }),
                     "a departed peer should drop out of the roster");
    passed &= expect(one->knows(2) && one->peerCount() == 2,
                     "a peer that is still talking should stay in the roster");

    passed &= expect(one->lastError().empty(),
                     "a healthy mesh should report no transport error: " + one->lastError());

    return passed;
}

// The point of the heartbeat: a peer that publishes no game state at all still
// belongs to the session. Before it existed, liveness was a side effect of a
// game publishing its player, so a paused game — or one between levels, or one
// whose player was simply standing still — was indistinguishable from a
// crashed one and was thrown out after five seconds.
//
// This test is deliberately slower than the timeout it is testing. There is no
// way to prove something does not happen after five seconds in less than five
// seconds, and the alternative — making the timeout configurable purely so the
// test can hurry it along — would be a knob that exists for the test rather
// than for any game.
bool testSilentPeersStayInTheSession()
{
    bool passed = true;

    auto makeConfig = [](Peer::PeerId id, const std::vector<std::string>& bootstrap) {
        Peer::PeerSession::Config config;
        config.id = id;
        config.name = "quiet" + std::to_string(id);
        config.pubBind = "tcp://127.0.0.1:*";
        config.greetBind = "tcp://127.0.0.1:*";
        config.advertiseHost = "127.0.0.1";
        config.bootstrap = bootstrap;
        return config;
    };

    Peer::PeerSession one(makeConfig(1, {}));
    one.start();
    Peer::PeerSession two(makeConfig(2, {one.self().greetEndpoint}));
    two.start();

    passed &= expect(waitUntil([&] { return one.peerCount() == 2 && two.peerCount() == 2; }),
                     "the two peers should find each other");

    // Neither publishes anything at all from here on. Well past the timeout.
    const auto quietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(7000);
    while (std::chrono::steady_clock::now() < quietUntil) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!one.knows(2) || !two.knows(1)) {
            break;
        }
    }

    passed &= expect(one.knows(2) && two.knows(1),
                     "a peer that publishes nothing should still be in the session");
    passed &= expect(one.peerCount() == 2 && two.peerCount() == 2,
                     "a silent peer should still be counted");

    // And the inbox should not have filled up with liveness traffic: a ping
    // has done its job by the time the game sees anything.
    for (const Peer::Envelope& envelope : one.drain()) {
        passed &= expect(envelope.type != Peer::MessageType::Ping,
                         "a game should never be handed a ping");
    }

    return passed;
}

} // namespace

int main()
{
    bool passed = true;

    passed &= testProtocolRoundTrips();
    passed &= testProtocolRejectsBadMessages();
    passed &= testThreePeerMeshFormsFromOneAddress();
    passed &= testSilentPeersStayInTheSession();

    if (passed) {
        std::cout << "peer-tests: all checks passed\n";
    }
    return passed ? 0 : 1;
}
