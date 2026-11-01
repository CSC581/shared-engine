#include "BinaryNetworkProtocol.hpp"
#include "EventCodec.hpp"
#include "EventManager.hpp"
#include "Multiplayer.hpp"
#include "NetworkClient.hpp"
#include "NetworkEventBridge.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkServer.hpp"
#include "NetworkServerHost.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "ZmqMessage.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Milestone 4, 1.2: events shared through the server. The server tests drive
// NetworkServer directly on a ManualClock; the client test answers a real
// NetworkClient from a hand-run REP socket; the host tests use the real
// NetworkServerHost on loopback.
namespace {

using Clock = std::chrono::steady_clock;

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit)
{
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

Network::SessionToken token(char digit)
{
    return Network::SessionToken(32, digit);
}

Event ping(std::int64_t n)
{
    Event event("Ping");
    event.set("n", n);
    return event;
}

std::vector<Network::OutgoingEvent> outgoing(std::uint64_t first, std::int64_t count)
{
    std::vector<Network::OutgoingEvent> events;
    for (std::int64_t i = 0; i < count; ++i) {
        events.push_back({first + static_cast<std::uint64_t>(i), ping(static_cast<std::int64_t>(first) + i)});
    }
    return events;
}

std::vector<std::int64_t> numbers(const std::vector<Network::RelayedEvent>& events)
{
    std::vector<std::int64_t> result;
    for (const Network::RelayedEvent& relayed : events) {
        result.push_back(relayed.event.getOr<std::int64_t>("n", -1));
    }
    return result;
}

Network::ServerConfig serverConfig()
{
    Network::ServerConfig config;
    config.spawnPoints = {{10.0F, 20.0F}, {30.0F, 20.0F}, {50.0F, 20.0F}};
    return config;
}

// A player on a NetworkServer driven through its typed boundary.
struct TestPlayer {
    Network::NetworkServer& server;
    Network::SessionToken sessionToken;
    Network::PlayerId id = 0;
    std::uint64_t positionSequence = 0;

    Network::Reply join()
    {
        Network::Request request;
        request.type = Network::RequestType::Join;
        request.sessionToken = sessionToken;
        Network::Reply reply = server.handle(request);
        id = reply.playerId;
        return reply;
    }

    Network::Reply send(std::vector<Network::OutgoingEvent> events, std::uint64_t ack = 0)
    {
        Network::Request request;
        request.type = Network::RequestType::Position;
        request.playerId = id;
        request.sessionToken = sessionToken;
        request.position.sequence = ++positionSequence;
        request.events = std::move(events);
        request.ackedEventSequence = ack;
        return server.handle(std::move(request));
    }
};

// --- Server ------------------------------------------------------------------

bool serverRelaysToEveryoneElseOnce()
{
    ManualClock clock;
    Network::NetworkServer server(clock, serverConfig());
    TestPlayer a{server, token('a')};
    TestPlayer b{server, token('b')};
    TestPlayer c{server, token('c')};
    a.join();
    b.join();
    c.join();

    std::vector<Network::OutgoingEvent> sent = outgoing(1, 2);
    sent[0].event.priority = EventPriority::High;
    sent[0].event.timestamp = 1234;
    sent[0].event.set("who", "ada");
    const Network::Reply fromA = a.send(sent);
    bool passed = expect(fromA.type == Network::ReplyType::Snapshot && fromA.acceptedClientSequence == 2 &&
                             fromA.events.empty(),
                         "the sender learns both events were accepted and never gets them back");

    const Network::Reply toB = b.send({});
    const Network::Reply toC = c.send({});
    for (const Network::Reply* reply : {&toB, &toC}) {
        passed &= expect(numbers(reply->events) == std::vector<std::int64_t>{1, 2},
                         "every other player gets the events, in order");
        if (reply->events.size() != 2) {
            continue;
        }
        const Event& first = reply->events[0].event;
        passed &= expect(first.origin == a.id && reply->events[1].event.origin == a.id,
                         "the server sets origin to the sender's id");
        passed &= expect(first.priority == EventPriority::High && first.timestamp == 1234 &&
                             first.getOr<std::string>("who", "") == "ada",
                         "priority, the sender's timestamp and arguments are relayed unchanged");
        passed &= expect(reply->events[0].serverSequence < reply->events[1].serverSequence,
                         "server sequence numbers increase");
    }
    passed &= expect(toB.events.size() == 2 && toC.events.size() == 2 &&
                         toB.events[0].serverSequence == toC.events[0].serverSequence,
                     "every receiver sees the same server sequence for an event");

    const std::vector<Network::PlayerTraffic> traffic = server.traffic();
    passed &= expect(traffic.size() == 3 && traffic[0].acceptedEvents == 2 && traffic[1].acceptedEvents == 0,
                     "traffic counts the events accepted from each player");
    return passed;
}

bool resentEventsAreRelayedOnce()
{
    ManualClock clock;
    Network::NetworkServer server(clock, serverConfig());
    TestPlayer a{server, token('a')};
    TestPlayer b{server, token('b')};
    a.join();
    b.join();

    a.send(outgoing(1, 2));
    // The reply was lost, so the client sends its events again with one more.
    const Network::Reply again = a.send(outgoing(1, 3));
    bool passed = expect(again.type == Network::ReplyType::Snapshot && again.acceptedClientSequence == 3,
                         "a resent list is accepted up to its newest event");
    passed &= expect(numbers(b.send({}).events) == std::vector<std::int64_t>{1, 2, 3},
                     "events sent twice are relayed once");

    // The very same request handled twice: refused the second time, so its
    // events cannot be relayed again.
    Network::Request request;
    request.type = Network::RequestType::Position;
    request.playerId = a.id;
    request.sessionToken = a.sessionToken;
    request.position.sequence = ++a.positionSequence;
    request.events = outgoing(4, 1);
    passed &= expect(server.handle(request).type == Network::ReplyType::Snapshot &&
                         server.handle(request).type == Network::ReplyType::Error,
                     "a repeated request is refused for its stale position sequence");
    passed &= expect(numbers(b.send({}).events) == std::vector<std::int64_t>{1, 2, 3, 4},
                     "a repeated request relays its events once");
    return passed;
}

bool acknowledgementsClearTheOutbox()
{
    ManualClock clock;
    Network::NetworkServer server(clock, serverConfig());
    TestPlayer a{server, token('a')};
    TestPlayer b{server, token('b')};
    a.join();
    b.join();
    a.send(outgoing(1, 3));

    const Network::Reply first = b.send({}, 0);
    const Network::Reply resent = b.send({}, 0);
    bool passed = expect(first.events.size() == 3 && numbers(resent.events) == numbers(first.events),
                         "unacknowledged events are sent again");
    const std::uint64_t secondSequence = first.events[1].serverSequence;
    passed &= expect(numbers(b.send({}, secondSequence).events) == std::vector<std::int64_t>{3},
                     "acknowledged events are dropped, later ones are kept");
    passed &= expect(b.send({}, first.events[2].serverSequence).events.empty(),
                     "once everything is acknowledged nothing is resent");
    return passed;
}

bool repliesCarryAtMostSixtyFourEvents()
{
    ManualClock clock;
    Network::NetworkServer server(clock, serverConfig());
    TestPlayer a{server, token('a')};
    TestPlayer b{server, token('b')};
    a.join();
    b.join();
    a.send(outgoing(1, 64));
    a.send(outgoing(65, 36));

    const Network::Reply first = b.send({});
    bool passed = expect(first.events.size() == 64 && numbers(first.events).front() == 1,
                         "one reply carries the oldest 64 events");
    const Network::Reply rest = b.send({}, first.events.back().serverSequence);
    passed &= expect(rest.events.size() == 36 && numbers(rest.events).front() == 65 &&
                         numbers(rest.events).back() == 100,
                     "the rest follow once the first batch is acknowledged");
    return passed;
}

bool fullOutboxDropsTheOldestAndCountsThem()
{
    ManualClock clock;
    Network::ServerConfig config = serverConfig();
    config.eventOutboxLimit = 3;
    Network::NetworkServer server(clock, config);
    TestPlayer a{server, token('a')};
    TestPlayer b{server, token('b')};
    a.join();
    b.join();
    a.send(outgoing(1, 5));

    bool passed = expect(numbers(b.send({}).events) == std::vector<std::int64_t>{3, 4, 5},
                         "a player that falls behind keeps only the newest events");
    const std::vector<Network::PlayerTraffic> traffic = server.traffic();
    passed &= expect(traffic.size() == 2 && traffic[0].acceptedEvents == 5 && traffic[0].droppedEvents == 0 &&
                         traffic[1].droppedEvents == 2,
                     "dropped events are counted for the player that missed them");

    Network::ServerConfig zero = serverConfig();
    zero.eventOutboxLimit = 0;
    bool threw = false;
    try {
        Network::NetworkServer invalid(clock, zero);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    passed &= expect(threw, "an outbox limit of 0 is rejected");
    return passed;
}

bool aSkippedEventIsRefused()
{
    ManualClock clock;
    Network::NetworkServer server(clock, serverConfig());
    TestPlayer a{server, token('a')};
    TestPlayer b{server, token('b')};
    a.join();
    b.join();
    const float before = server.snapshot().players[0].x;

    Network::Request request;
    request.type = Network::RequestType::Position;
    request.playerId = a.id;
    request.sessionToken = a.sessionToken;
    request.position = {999.0F, 0.0F, 1, {}};
    request.events = outgoing(2, 1);
    bool passed = expect(server.handle(request).type == Network::ReplyType::Error,
                         "an event list that skips a sequence number is refused");
    passed &= expect(server.snapshot().players[0].x == before && b.send({}).events.empty(),
                     "a refused request moves nothing and relays nothing");
    return passed;
}

bool rejoiningKeepsTheOutboxAndSequences()
{
    ManualClock clock;
    Network::NetworkServer server(clock, serverConfig());
    TestPlayer a{server, token('a')};
    TestPlayer b{server, token('b')};
    const Network::Reply firstA = a.join();
    const Network::Reply firstB = b.join();
    bool passed = expect(firstA.acceptedClientSequence == 0 && firstA.serverInstance != 0 &&
                             firstB.serverInstance == firstA.serverInstance,
                         "a first WELCOME has nothing accepted and names the server instance");
    a.send(outgoing(1, 2));

    const Network::PlayerId oldId = b.id;
    const Network::Reply welcome = b.join();
    passed &= expect(welcome.type == Network::ReplyType::Welcome && b.id == oldId &&
                         welcome.serverInstance == firstA.serverInstance,
                     "a JOIN with a live token resumes the same player on the same instance");
    passed &= expect(numbers(b.send({}).events) == std::vector<std::int64_t>{1, 2},
                     "the outbox survives the rejoin");
    passed &= expect(a.join().acceptedClientSequence == 2, "a resumed WELCOME says what was accepted");
    passed &= expect(a.send(outgoing(1, 3)).acceptedClientSequence == 3 &&
                         numbers(b.send({}).events) == std::vector<std::int64_t>{1, 2, 3},
                     "the sender's accepted sequence survives its rejoin too");

    // Timed out: the token is forgotten, so the next JOIN is a new player.
    clock.advance(serverConfig().inactivityTimeoutTics);
    server.update();
    const Network::Reply fresh = a.join();
    passed &= expect(fresh.type == Network::ReplyType::Welcome && fresh.acceptedClientSequence == 0 &&
                         a.id != oldId && fresh.serverInstance == firstA.serverInstance,
                     "a JOIN after a timeout is a new player with nothing accepted");
    // Its WELCOME was lost, so the client joins again: same new player, and
    // still nothing accepted, so the client renumbers either way.
    const Network::PlayerId newId = a.id;
    const Network::Reply retried = a.join();
    passed &= expect(retried.acceptedClientSequence == 0 && a.id == newId,
                     "a retried JOIN for the new player still reports nothing accepted");
    passed &= expect(a.send(outgoing(1, 1)).acceptedClientSequence == 1,
                     "a new player numbers its events from 1 again");

    Network::NetworkServer restarted(clock, serverConfig());
    TestPlayer c{restarted, token('c')};
    passed &= expect(c.join().serverInstance != firstA.serverInstance,
                     "another server (a restart) has another instance");
    return passed;
}

// --- Wire formats ---------------------------------------------------------

Network::Request positionWithEvents()
{
    Network::Request request;
    request.type = Network::RequestType::Position;
    request.playerId = 4;
    request.sessionToken = token('a');
    request.position = {1.5F, -2.0F, 9, "data"};
    request.ackedEventSequence = 77;
    request.events = outgoing(3, 2);
    request.events[1].event.set("x", 0.25).set("ok", true).set("who", "ada");
    return request;
}

Network::Reply snapshotWithEvents()
{
    Network::Reply reply;
    reply.type = Network::ReplyType::Snapshot;
    reply.snapshot.serverTick = 12;
    reply.snapshot.players = {{4, "ada", 1.0F, 2.0F, {}}};
    reply.snapshot.platforms = {{7, 0.0F, 100.0F, 96.0F, 20.0F}};
    reply.acceptedClientSequence = 5;
    for (std::uint64_t i = 0; i < 2; ++i) {
        Network::RelayedEvent relayed{10 + i * 3, ping(static_cast<std::int64_t>(i))};
        relayed.event.origin = 2;
        relayed.event.timestamp = 50;
        reply.events.push_back(relayed);
    }
    return reply;
}

bool sameRequest(const Network::Request& a, const Network::Request& b)
{
    if (a.events.size() != b.events.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        if (a.events[i].clientSequence != b.events[i].clientSequence ||
            a.events[i].event.args != b.events[i].event.args || a.events[i].event.type != b.events[i].event.type) {
            return false;
        }
    }
    return a.ackedEventSequence == b.ackedEventSequence && a.position.sequence == b.position.sequence &&
           a.position.data == b.position.data;
}

bool sameEvents(const std::vector<Network::RelayedEvent>& a, const std::vector<Network::RelayedEvent>& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const auto& left, const auto& right) {
               return left.serverSequence == right.serverSequence && left.event.type == right.event.type &&
                      left.event.args == right.event.args && left.event.origin == right.event.origin &&
                      left.event.timestamp == right.event.timestamp;
           });
}

bool bothFormatsCarryEventsAndAcks()
{
    std::string error;
    const Network::Request request = positionWithEvents();
    const Network::Reply reply = snapshotWithEvents();

    Network::Request text;
    bool passed = expect(Network::decodeRequest(Network::encodePosition(request.playerId, request.sessionToken,
                                                                       request.position, request.ackedEventSequence,
                                                                       request.events),
                                                text, error) &&
                             sameRequest(request, text),
                         "a text POSITION round-trips its acknowledgement and events: " + error);
    Network::Request binary;
    passed &= expect(Network::decodeBinaryRequest(Network::encodeBinaryRequest(request), binary, error) &&
                         sameRequest(request, binary),
                     "a binary POSITION round-trips its acknowledgement and events: " + error);
    Network::BinaryRequestEnvelope deltaRequest;
    deltaRequest.request = request;
    deltaRequest.snapshotStrategy = Net::SnapshotStrategy::PropertyDelta;
    deltaRequest.acknowledgedServerTick = 3;
    Network::BinaryRequestEnvelope deltaDecoded;
    passed &= expect(Network::decodeBinaryRequest(Network::encodeBinaryRequest(deltaRequest), deltaDecoded, error) &&
                         sameRequest(request, deltaDecoded.request),
                     "a property-delta POSITION round-trips its events: " + error);

    Network::Reply textReply;
    passed &= expect(Network::decodeReply(Network::encodeReply(reply), textReply, error) &&
                         textReply.acceptedClientSequence == 5 && sameEvents(reply.events, textReply.events) &&
                         textReply.snapshot.platforms.size() == 1,
                     "a text SNAPSHOT round-trips acceptance and events: " + error);
    Network::Reply binaryReply;
    passed &= expect(Network::decodeBinaryReply(Network::encodeBinaryReply(reply), binaryReply, error) &&
                         binaryReply.acceptedClientSequence == 5 && sameEvents(reply.events, binaryReply.events),
                     "a binary SNAPSHOT round-trips acceptance and events: " + error);

    Network::BinaryReplyEnvelope delta;
    delta.kind = Network::BinaryReplyKind::SnapshotDelta;
    delta.reply = reply;
    Network::WorldSnapshot base = reply.snapshot;
    base.serverTick = 11;
    delta.delta = Network::makeSnapshotDelta(base, reply.snapshot);
    Network::BinaryReplyEnvelope deltaReply;
    passed &= expect(Network::decodeBinaryReply(Network::encodeBinaryReply(delta), deltaReply, error) &&
                         deltaReply.kind == Network::BinaryReplyKind::SnapshotDelta &&
                         deltaReply.reply.acceptedClientSequence == 5 && sameEvents(reply.events, deltaReply.reply.events),
                     "a snapshot delta carries acceptance and events too: " + error);

    Network::Reply welcome;
    welcome.type = Network::ReplyType::Welcome;
    welcome.playerId = 4;
    welcome.acceptedClientSequence = 17;
    welcome.serverInstance = 0xFEDCBA9876543210ULL;
    Network::Reply decodedText;
    Network::Reply decodedBinary;
    Network::BinaryReplyEnvelope deltaWelcome;
    deltaWelcome.kind = Network::BinaryReplyKind::DeltaWelcome;
    deltaWelcome.reply = welcome;
    Network::BinaryReplyEnvelope decodedDelta;
    passed &= expect(Network::decodeReply(Network::encodeReply(welcome), decodedText, error) &&
                         decodedText.acceptedClientSequence == 17 &&
                         decodedText.serverInstance == welcome.serverInstance &&
                         Network::decodeBinaryReply(Network::encodeBinaryReply(welcome), decodedBinary, error) &&
                         decodedBinary.acceptedClientSequence == 17 &&
                         decodedBinary.serverInstance == welcome.serverInstance &&
                         Network::decodeBinaryReply(Network::encodeBinaryReply(deltaWelcome), decodedDelta, error) &&
                         decodedDelta.reply.acceptedClientSequence == 17 &&
                         decodedDelta.reply.serverInstance == welcome.serverInstance,
                     "WELCOME carries what was accepted and the server instance in every format: " + error);
    return passed;
}

bool decodersRejectBrokenEventFields()
{
    std::string error;
    Network::Request request;
    Network::Reply reply;
    const Network::Message position =
        Network::encodePosition(4, token('a'), {1.0F, 2.0F, 1, {}}, 0, outgoing(1, 1));

    Network::Message noEvents(position.begin(), position.begin() + 8);
    bool passed = expect(!Network::decodeRequest(noEvents, request, error),
                         "a POSITION without acknowledgement and events is refused");
    Network::Message badAck = position;
    badAck[8] = "-1";
    passed &= expect(!Network::decodeRequest(badAck, request, error), "a malformed acknowledgement is refused");
    Network::Message trailing = position;
    trailing.push_back("extra");
    passed &= expect(!Network::decodeRequest(trailing, request, error), "fields after the events are refused");
    Network::Message truncated = position;
    truncated.pop_back();
    passed &= expect(!Network::decodeRequest(truncated, request, error), "a truncated event list is refused");

    const Network::Message snapshot = Network::encodeReply(snapshotWithEvents());
    Network::Message cut(snapshot.begin(), snapshot.end() - 1);
    passed &= expect(!Network::decodeReply(cut, reply, error), "a truncated SNAPSHOT event list is refused");
    Network::Message extra = snapshot;
    extra.push_back("0");
    passed &= expect(!Network::decodeReply(extra, reply, error), "fields after SNAPSHOT events are refused");

    Network::Reply welcome;
    welcome.type = Network::ReplyType::Welcome;
    welcome.playerId = 4;
    Network::Message badInstance = Network::encodeReply(welcome);
    badInstance.back() = "x";
    passed &= expect(!Network::decodeReply(badInstance, reply, error), "a malformed server instance is refused");
    Network::Message shortWelcome = Network::encodeReply(welcome);
    shortWelcome.pop_back();
    passed &= expect(!Network::decodeReply(shortWelcome, reply, error),
                     "a WELCOME without its server instance is refused");

    const Network::BinaryMessage bytes = Network::encodeBinaryReply(snapshotWithEvents());
    passed &= expect(!Network::decodeBinaryReply(bytes.substr(0, bytes.size() - 1), reply, error),
                     "a truncated binary SNAPSHOT is refused");
    const Network::BinaryMessage binaryWelcome = Network::encodeBinaryReply(welcome);
    passed &= expect(!Network::decodeBinaryReply(binaryWelcome.substr(0, binaryWelcome.size() - 1), reply, error),
                     "a truncated binary WELCOME is refused");

    // Every shorter prefix of a binary message carrying events is refused.
    Network::BinaryRequestEnvelope deltaRequest;
    deltaRequest.request = positionWithEvents();
    deltaRequest.snapshotStrategy = Net::SnapshotStrategy::PropertyDelta;
    deltaRequest.acknowledgedServerTick = 3;
    Network::BinaryReplyEnvelope deltaReply;
    deltaReply.kind = Network::BinaryReplyKind::SnapshotDelta;
    deltaReply.reply = snapshotWithEvents();
    Network::WorldSnapshot base = deltaReply.reply.snapshot;
    base.serverTick = 11;
    deltaReply.delta = Network::makeSnapshotDelta(base, deltaReply.reply.snapshot);
    const Network::BinaryMessage whole[] = {
        Network::encodeBinaryRequest(positionWithEvents()),
        Network::encodeBinaryRequest(deltaRequest),
        Network::encodeBinaryReply(snapshotWithEvents()),
        Network::encodeBinaryReply(deltaReply),
    };
    bool everyPrefixRefused = true;
    for (std::size_t kind = 0; kind < 4; ++kind) {
        for (std::size_t length = 0; length < whole[kind].size(); ++length) {
            const Network::BinaryMessage prefix = whole[kind].substr(0, length);
            Network::BinaryRequestEnvelope requestEnvelope;
            Network::BinaryReplyEnvelope replyEnvelope;
            everyPrefixRefused &= kind < 2 ? !Network::decodeBinaryRequest(prefix, requestEnvelope, error)
                                           : !Network::decodeBinaryReply(prefix, replyEnvelope, error);
        }
    }
    passed &= expect(everyPrefixRefused, "every truncated binary message carrying events is refused");

    // A relayed event decoded from a client's request would let a client
    // choose its own origin; the direction of each list is fixed.
    Network::Message relayedInRequest(position.begin(), position.begin() + 9);
    Network::appendEventsText(relayedInRequest, snapshotWithEvents().events);
    passed &= expect(!Network::decodeRequest(relayedInRequest, request, error),
                     "a POSITION cannot carry server-to-client events");
    return passed;
}

// --- Client -------------------------------------------------------------------

// Answers a NetworkClient from a REP socket the test runs by hand, so the test
// decides exactly what the "server" says.
struct ScriptedServer {
    zmq::context_t context{1};
    zmq::socket_t socket{context, zmq::socket_type::rep};
    std::string endpoint;

    ScriptedServer()
    {
        socket.set(zmq::sockopt::linger, 0);
        socket.set(zmq::sockopt::rcvtimeo, 2000);
        socket.bind("tcp://127.0.0.1:*");
        endpoint = socket.get(zmq::sockopt::last_endpoint);
    }

    bool receive(Network::Request& request)
    {
        bool received = false;
        const Network::Message message = Net::receive(socket, received);
        std::string error;
        return received && Network::decodeRequest(message, request, error);
    }

    void reply(const Network::Reply& reply) { Net::send(socket, Network::encodeReply(reply)); }
};

// Polls until the client has read the reply to its last request.
bool settle(Network::NetworkClient& client)
{
    return waitFor(
        [&] {
            client.poll();
            return !client.awaitingReply();
        },
        std::chrono::milliseconds(2000));
}

Network::Reply welcome(Network::PlayerId id, std::uint64_t accepted = 0, std::uint64_t instance = 1)
{
    Network::Reply reply;
    reply.type = Network::ReplyType::Welcome;
    reply.playerId = id;
    reply.acceptedClientSequence = accepted;
    reply.serverInstance = instance;
    return reply;
}

Network::Reply snapshotReply(std::uint64_t tick, std::uint64_t accepted,
                             std::vector<std::pair<std::uint64_t, std::int64_t>> relayed = {})
{
    Network::Reply reply;
    reply.type = Network::ReplyType::Snapshot;
    reply.snapshot.serverTick = tick;
    reply.acceptedClientSequence = accepted;
    for (const auto& [sequence, n] : relayed) {
        Network::RelayedEvent event{sequence, ping(n)};
        event.event.origin = 9;
        reply.events.push_back(event);
    }
    return reply;
}

std::vector<std::uint64_t> clientSequences(const Network::Request& request)
{
    std::vector<std::uint64_t> result;
    for (const Network::OutgoingEvent& event : request.events) {
        result.push_back(event.clientSequence);
    }
    return result;
}

bool clientResendsUntilAcceptedAndHandlesEachEventOnce()
{
    ScriptedServer server;
    ManualClock clock;
    Network::NetworkClient client(clock, server.endpoint);
    client.publishEvent(ping(1));
    client.publishEvent(ping(2));
    client.start();

    Network::Request request;
    bool passed = expect(server.receive(request) && request.type == Network::RequestType::Join, "client joins");
    server.reply(welcome(5));
    passed &= expect(settle(client) && client.state() == Network::ConnectionState::Connected, "client connects");

    client.submitPosition(1.0F, 1.0F);
    passed &= expect(server.receive(request) && clientSequences(request) == std::vector<std::uint64_t>{1, 2} &&
                         request.ackedEventSequence == 0,
                     "events queued before connecting ride on the first POSITION");
    server.reply(snapshotReply(1, 1, {{5, 50}}));
    passed &= expect(settle(client), "client reads the reply");
    passed &= expect(client.pendingEvents() == 1, "an accepted event leaves the queue, the other stays");

    client.submitPosition(1.0F, 1.0F);
    passed &= expect(server.receive(request) && clientSequences(request) == std::vector<std::uint64_t>{2} &&
                         request.ackedEventSequence == 5,
                     "an unaccepted event is sent again, with the relayed sequence acknowledged");
    // The server resends 5 (as if the acknowledgement had been lost) and adds 6.
    server.reply(snapshotReply(2, 2, {{5, 50}, {6, 60}}));
    passed &= expect(settle(client), "client reads the second reply");
    const std::vector<Event> received = client.takeEvents();
    passed &= expect(received.size() == 2 && received[0].getOr<std::int64_t>("n", 0) == 50 &&
                         received[1].getOr<std::int64_t>("n", 0) == 60 && received[0].origin == 9,
                     "each relayed event is handed on once, in order, with its origin");
    passed &= expect(client.pendingEvents() == 0 && client.takeEvents().empty(), "nothing is left over");

    bool threw = false;
    try {
        client.publishEvent(Event("bad name"));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    passed &= expect(threw && client.pendingEvents() == 0, "an event that cannot be sent is refused at once");
    return passed;
}

// After a rejoin the client follows the WELCOME: it drops what the server
// accepted, numbers the rest on from there, and forgets relayed sequence
// numbers only when the server instance changed.
bool clientKeepsOrRenumbersEventsOnRejoin()
{
    struct Case {
        const char* label;
        Network::Reply welcome;
        std::vector<std::uint64_t> resent;
        std::uint64_t ack;
        std::uint64_t accepted;
        std::uint64_t relayed;
        bool handled;
    };
    const Case cases[] = {
        // Same player: numbers and acknowledgement carry on; relayed 2 is old.
        {"resumed: ", welcome(5, 1, 1), {2, 3}, 8, 3, 2, false},
        // New player on the same server, or a retried JOIN after a lost
        // WELCOME: renumbered, but the same server's sequence numbers stand.
        {"new player, same server: ", welcome(6, 0, 1), {1, 2}, 8, 2, 9, true},
        // Restarted server: renumbered, and its numbering starts again.
        {"restarted server: ", welcome(6, 0, 2), {1, 2}, 0, 2, 2, true},
    };
    bool passed = true;
    for (const Case& test : cases) {
        const std::string label = test.label;
        ScriptedServer server;
        ManualClock clock;
        Network::NetworkClient client(clock, server.endpoint);
        client.start();
        Network::Request request;
        server.receive(request);
        server.reply(welcome(5));
        settle(client);

        client.publishEvent(ping(1));
        client.publishEvent(ping(2));
        client.publishEvent(ping(3));
        client.submitPosition(0.0F, 0.0F);
        server.receive(request);
        server.reply(snapshotReply(1, 1, {{8, 80}}));
        settle(client);
        client.takeEvents();

        // The server drops the player: the client retries and joins again.
        client.submitPosition(0.0F, 0.0F);
        server.receive(request);
        Network::Reply refused;
        refused.type = Network::ReplyType::Error;
        refused.error = "unknown player ID";
        server.reply(refused);
        settle(client);
        clock.advance(kNsPerSec);
        client.poll();
        passed &= expect(server.receive(request) && request.type == Network::RequestType::Join,
                         label + "the client joins again with its token");
        server.reply(test.welcome);
        settle(client);

        client.submitPosition(0.0F, 0.0F);
        passed &= expect(server.receive(request) && clientSequences(request) == test.resent &&
                             request.ackedEventSequence == test.ack &&
                             request.playerId == test.welcome.playerId,
                         label + "waiting events are numbered and acknowledged as the WELCOME says");
        server.reply(snapshotReply(1, test.accepted, {{test.relayed, 20}}));
        settle(client);
        const std::vector<Event> received = client.takeEvents();
        passed &= expect(received.size() == (test.handled ? 1U : 0U),
                         label + "relayed events are handled once, by the right numbering");
        passed &= expect(client.pendingEvents() == 0, label + "everything was accepted in the end");
    }
    return passed;
}

std::vector<std::int64_t> eventNumbers(const Network::Request& request)
{
    std::vector<std::int64_t> result;
    for (const Network::OutgoingEvent& event : request.events) {
        result.push_back(event.event.getOr<std::int64_t>("n", -1));
    }
    return result;
}

std::vector<std::int64_t> range(std::int64_t first, std::int64_t count)
{
    std::vector<std::int64_t> result;
    for (std::int64_t i = 0; i < count; ++i) {
        result.push_back(first + i);
    }
    return result;
}

std::vector<std::uint64_t> sequenceRange(std::uint64_t first, std::uint64_t count)
{
    std::vector<std::uint64_t> result;
    for (std::uint64_t i = 0; i < count; ++i) {
        result.push_back(first + i);
    }
    return result;
}

// A full queue drops its oldest unsent event and renumbers the rest, but never
// touches events already sent, which the server may have accepted.
bool fullClientQueueDropsTheOldestUnsentEvents()
{
    constexpr auto cap = static_cast<std::int64_t>(Network::NetworkClient::maxPendingEvents);
    ScriptedServer server;
    ManualClock clock;
    Network::NetworkClient client(clock, server.endpoint);
    for (std::int64_t n = 0; n < cap + 44; ++n) {
        client.publishEvent(ping(n));
    }
    bool passed = expect(client.pendingEvents() == Network::NetworkClient::maxPendingEvents &&
                             client.droppedEvents() == 44,
                         "a full queue keeps the newest events and counts what it dropped");

    client.start();
    Network::Request request;
    server.receive(request);
    server.reply(welcome(5));
    settle(client);
    client.submitPosition(0.0F, 0.0F);
    passed &= expect(server.receive(request) && clientSequences(request) == sequenceRange(1, 64) &&
                         eventNumbers(request) == range(44, 64),
                     "the queue is still numbered from 1 after drops");

    // While that request is unanswered, its 64 events may already be
    // accepted, so new arrivals push out only events that were never sent.
    for (std::int64_t n = 1000; n < 1000 + cap + 44; ++n) {
        client.publishEvent(ping(n));
    }
    passed &= expect(client.pendingEvents() == Network::NetworkClient::maxPendingEvents &&
                         client.droppedEvents() == 44 + cap + 44,
                     "the queue stays capped while a request is in flight");
    server.reply(snapshotReply(1, 64));
    settle(client);
    client.submitPosition(0.0F, 0.0F);
    passed &= expect(server.receive(request) && clientSequences(request) == sequenceRange(65, 64) &&
                         eventNumbers(request) == range(1000 + cap + 44 - (cap - 64), 64),
                     "events sent before keep their numbers and the rest follow on consecutively");
    server.reply(snapshotReply(2, 128));
    settle(client);
    passed &= expect(client.pendingEvents() == static_cast<std::size_t>(cap - 128),
                     "the server's acceptance matches what the client renumbered");
    return passed;
}

// Relayed events a caller never takes stop at the cap, keeping the newest.
bool clientIncomingQueueIsCapped()
{
    ScriptedServer server;
    ManualClock clock;
    Network::NetworkClient client(clock, server.endpoint);
    client.start();
    Network::Request request;
    server.receive(request);
    server.reply(welcome(5));
    settle(client);

    std::uint64_t sequence = 0;
    std::int64_t n = 0;
    for (std::uint64_t tick = 1; tick <= 5; ++tick) {
        std::vector<std::pair<std::uint64_t, std::int64_t>> relayed;
        for (int i = 0; i < 64; ++i) {
            relayed.push_back({++sequence, n++});
        }
        client.submitPosition(0.0F, 0.0F);
        server.receive(request);
        server.reply(snapshotReply(tick, 0, relayed));
        settle(client);
    }
    constexpr auto cap = static_cast<std::int64_t>(Network::NetworkClient::maxPendingEvents);
    const std::vector<Event> received = client.takeEvents();
    std::vector<std::int64_t> values;
    for (const Event& event : received) {
        values.push_back(event.getOr<std::int64_t>("n", -1));
    }
    bool passed = expect(values == range(5 * 64 - cap, cap) && client.droppedEvents() == 5 * 64 - cap,
                         "untaken relayed events stop at the cap, keeping the newest and counting the rest");
    client.submitPosition(0.0F, 0.0F);
    passed &= expect(server.receive(request) && request.ackedEventSequence == sequence,
                     "dropped events are still acknowledged, so the server does not resend them");
    return passed;
}

// --- Real host ----------------------------------------------------------------

Network::HostConfig hostConfig()
{
    Network::HostConfig config;
    config.bindEndpoint = "tcp://127.0.0.1:*";
    config.advertiseHost = "127.0.0.1";
    config.pollInterval = std::chrono::milliseconds(20);
    return config;
}

// Text, binary and property-delta clients on one server. A and B send; every
// client receives everyone else's events once, and the two that only listen
// see the same order.
bool mixedClientsExchangeEventsThroughTheHost()
{
    Network::NetworkServerHost host(serverConfig(), hostConfig());
    host.start();
    RealTimeClock clock;

    struct Setup {
        Net::DataFormat format;
        Net::SnapshotStrategy strategy;
    };
    const Setup setups[] = {
        {Net::DataFormat::TextMultipart, Net::SnapshotStrategy::FullSnapshot},
        {Net::DataFormat::Binary, Net::SnapshotStrategy::FullSnapshot},
        {Net::DataFormat::Binary, Net::SnapshotStrategy::PropertyDelta},
        {Net::DataFormat::TextMultipart, Net::SnapshotStrategy::FullSnapshot},
    };
    std::vector<std::unique_ptr<Network::NetworkClient>> clients;
    for (const Setup& setup : setups) {
        clients.push_back(std::make_unique<Network::NetworkClient>(clock, host.boundEndpoint(), setup.format,
                                                                   setup.strategy));
        clients.back()->start();
    }
    std::vector<std::vector<Event>> received(clients.size());
    const auto pump = [&] {
        for (std::size_t i = 0; i < clients.size(); ++i) {
            clients[i]->poll();
            clients[i]->submitPosition(static_cast<float>(i), 0.0F);
            for (Event& event : clients[i]->takeEvents()) {
                received[i].push_back(std::move(event));
            }
        }
    };
    bool passed = expect(waitFor(
                             [&] {
                                 pump();
                                 return std::all_of(clients.begin(), clients.end(), [](const auto& client) {
                                     return client->state() == Network::ConnectionState::Connected;
                                 });
                             },
                             std::chrono::milliseconds(3000)),
                         "all four clients connect");

    constexpr std::int64_t perSender = 40;
    for (std::int64_t n = 0; n < perSender; ++n) {
        clients[0]->publishEvent(ping(n));
        clients[1]->publishEvent(ping(100 + n));
    }
    const auto complete = [&] {
        return received[0].size() == perSender && received[1].size() == perSender &&
               received[2].size() == 2 * perSender && received[3].size() == 2 * perSender;
    };
    passed &= expect(waitFor(
                         [&] {
                             pump();
                             return complete();
                         },
                         std::chrono::milliseconds(5000)),
                     "every client receives every other client's events");
    // Give any duplicate a chance to show up.
    for (int i = 0; i < 20; ++i) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    passed &= expect(complete(), "no event arrives twice");

    const auto values = [](const std::vector<Event>& events) {
        std::vector<std::int64_t> result;
        for (const Event& event : events) {
            result.push_back(event.getOr<std::int64_t>("n", -1));
        }
        return result;
    };
    std::vector<std::int64_t> fromA;
    std::vector<std::int64_t> fromB;
    for (std::int64_t n = 0; n < perSender; ++n) {
        fromA.push_back(n);
        fromB.push_back(100 + n);
    }
    passed &= expect(values(received[1]) == fromA && values(received[0]) == fromB,
                     "each sender's events arrive in the order they were sent, and never back at the sender");
    passed &= expect(values(received[2]) == values(received[3]),
                     "text and property-delta clients see the same global order");
    passed &= expect(std::all_of(received[0].begin(), received[0].end(),
                                 [&](const Event& event) { return event.origin == clients[1]->playerId(); }),
                     "relayed events carry the sender's id");
    passed &= expect(clients[0]->pendingEvents() == 0 && clients[1]->pendingEvents() == 0,
                     "every published event was accepted");
    return passed;
}

// --- Bridge -------------------------------------------------------------------

class FakeSession final : public Multiplayer::Session {
public:
    Multiplayer::Mode sessionMode = Multiplayer::Mode::ClientServer;
    std::vector<Event> published;
    std::vector<Event> incoming;

    void update() override {}
    void publishLocalPlayer(float, float, const std::string&) override {}
    void leave() override {}
    Multiplayer::PlayerId localPlayerId() const override { return 1; }
    const std::vector<Multiplayer::Player>& remotePlayers() const override { return players_; }
    const std::vector<Multiplayer::Platform>& platforms() const override { return platforms_; }
    Multiplayer::State state() const override { return Multiplayer::State::Ready; }
    Multiplayer::AuthorityState authorityState() const override { return Multiplayer::AuthorityState::Ready; }
    std::string status() const override { return "fake"; }
    Multiplayer::Mode mode() const override { return sessionMode; }
    void publishEvent(const Event& event) override { published.push_back(event); }
    std::vector<Event> takeRemoteEvents() override
    {
        std::vector<Event> events;
        events.swap(incoming);
        return events;
    }

private:
    std::vector<Multiplayer::Player> players_;
    std::vector<Multiplayer::Platform> platforms_;
};

bool bridgeSharesOnlyListedLocalEvents()
{
    ManualClock clock;
    EventManager events(clock);
    FakeSession session;
    std::vector<Event> handled;
    events.subscribeAll([&](const Event& event) { handled.push_back(event); });

    bool passed = true;
    {
        NetworkEventBridge bridge(events, session, {"Ping", "Ping"});
        clock.advance(500);
        events.raise(ping(1));
        events.raise(Event("Secret"));
        events.dispatch();
        passed &= expect(session.published.size() == 1 && session.published[0].type == "Ping" &&
                             session.published[0].timestamp == 500,
                         "only listed types are published, once each, with their timestamp");

        Event remote = ping(2);
        remote.origin = 7;
        remote.timestamp = 40;
        remote.priority = EventPriority::High;
        Event unlisted("InputEvent");
        unlisted.origin = 7;
        Event unmarked = ping(3);
        session.incoming = {remote, unlisted, unmarked};
        handled.clear();
        clock.advance(500);
        bridge.apply();
        events.dispatch();
        passed &= expect(handled.size() == 1 && handled[0].origin == 7 && handled[0].timestamp == 1000 &&
                             handled[0].getOr<std::int64_t>("sentAt", -1) == 40 &&
                             handled[0].priority == EventPriority::High,
                         "a remote event is raised with the local time, the sender's time as sentAt, its origin "
                         "and its priority; unlisted types and events without an origin are ignored");
        passed &= expect(session.published.size() == 1, "a remote event is never published again");
    }

    events.raise(ping(4));
    events.dispatch();
    passed &= expect(session.published.size() == 1, "a destroyed bridge publishes nothing");
    return passed;
}

bool bridgeRefusesWhatCannotWork()
{
    ManualClock clock;
    EventManager events(clock);
    FakeSession session;
    bool passed = true;

    session.sessionMode = Multiplayer::Mode::PeerToPeer;
    bool threw = false;
    try {
        NetworkEventBridge bridge(events, session, {"Ping"});
    } catch (const std::logic_error&) {
        threw = true;
    }
    passed &= expect(threw, "a bridge over a peer-to-peer session fails at once");

    session.sessionMode = Multiplayer::Mode::ClientServer;
    threw = false;
    try {
        NetworkEventBridge bridge(events, session, {"has space"});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    passed &= expect(threw, "a type that is not a valid event name is refused");

    Multiplayer::Config config;
    config.mode = Multiplayer::Mode::PeerToPeer;
    config.serverEndpoint.clear();
    config.basePort = 42150;
    RealTimeClock realTime;
    std::unique_ptr<Multiplayer::Session> peer = Multiplayer::Session::open(config, realTime);
    threw = false;
    try {
        peer->publishEvent(ping(1));
    } catch (const std::logic_error&) {
        threw = true;
    }
    passed &= expect(threw && peer->takeRemoteEvents().empty(),
                     "a peer-to-peer session refuses to publish and delivers nothing");
    return passed;
}

// With send pacing, a paused game sends a pose only at its heartbeat. Events
// must not wait for it: the session resends its last pose to carry them.
bool pacedSessionSendsEventsWithoutWaiting()
{
    Network::NetworkServerHost host(serverConfig(), hostConfig());
    host.start();
    RealTimeClock realTime;
    ManualClock frozenGameTime;

    Multiplayer::Config paced;
    paced.serverEndpoint = host.boundEndpoint();
    paced.sendIntervalGameTics = 1000 * kGameTicsPerSecond;
    paced.heartbeatIntervalRealTics = 1000 * kNsPerSec;
    std::unique_ptr<Multiplayer::Session> sender = Multiplayer::Session::open(paced, realTime, &frozenGameTime);
    Multiplayer::Config plain;
    plain.serverEndpoint = host.boundEndpoint();
    std::unique_ptr<Multiplayer::Session> receiver = Multiplayer::Session::open(plain, realTime);

    std::vector<Event> received;
    const auto frame = [&] {
        sender->update();
        receiver->update();
        sender->publishLocalPlayer(1.0F, 1.0F);
        receiver->publishLocalPlayer(2.0F, 2.0F);
        for (Event& event : receiver->takeRemoteEvents()) {
            received.push_back(std::move(event));
        }
    };
    bool passed = expect(waitFor(
                             [&] {
                                 frame();
                                 return sender->state() == Multiplayer::State::Ready &&
                                        receiver->remotePlayers().size() == 1;
                             },
                             std::chrono::milliseconds(2000)),
                         "the paced sender connects and is seen once");

    sender->publishEvent(ping(7));
    passed &= expect(waitFor(
                         [&] {
                             frame();
                             return received.size() == 1;
                         },
                         std::chrono::milliseconds(1000)),
                     "an event from a paced, paused game arrives without waiting for its heartbeat");
    passed &= expect(!received.empty() && received[0].getOr<std::int64_t>("n", 0) == 7,
                     "the event arrives intact");
    return passed;
}

// Two sessions on one host, connected and seeing each other.
struct SessionPair {
    Network::NetworkServerHost host{serverConfig(), hostConfig()};
    RealTimeClock realTime;
    std::unique_ptr<Multiplayer::Session> sender;
    std::unique_ptr<Multiplayer::Session> receiver;

    bool open()
    {
        host.start();
        Multiplayer::Config config;
        config.serverEndpoint = host.boundEndpoint();
        sender = Multiplayer::Session::open(config, realTime);
        receiver = Multiplayer::Session::open(config, realTime);
        return waitFor(
            [&] {
                frame();
                return sender->state() == Multiplayer::State::Ready &&
                       receiver->remotePlayers().size() == 1 && sender->remotePlayers().size() == 1;
            },
            std::chrono::milliseconds(2000));
    }

    // One game frame on both sides, without taking any events.
    void frame()
    {
        if (sender) {
            sender->update();
            sender->publishLocalPlayer(1.0F, 1.0F);
        }
        receiver->update();
        receiver->publishLocalPlayer(2.0F, 2.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Frames until the receiver takes an event, or the time runs out.
    std::vector<Event> receive(std::chrono::milliseconds limit)
    {
        std::vector<Event> taken;
        waitFor(
            [&] {
                frame();
                taken = receiver->takeRemoteEvents();
                return !taken.empty();
            },
            limit);
        return taken;
    }
};

bool sessionKeepsOnlyTheLatestFrameOfEvents()
{
    SessionPair pair;
    bool passed = expect(pair.open(), "both sessions connect");

    pair.sender->publishEvent(ping(1));
    const std::vector<Event> first = pair.receive(std::chrono::milliseconds(1000));
    passed &= expect(first.size() == 1 && first[0].getOr<std::int64_t>("n", 0) == 1, "an event arrives");

    // Delivered by some update() but never taken: gone by the next one.
    pair.sender->publishEvent(ping(2));
    const auto until = Clock::now() + std::chrono::milliseconds(500);
    while (Clock::now() < until) {
        pair.frame();
    }
    passed &= expect(pair.receiver->takeRemoteEvents().empty(),
                     "events not taken before the next update() are dropped, not piled up");

    pair.sender->publishEvent(ping(3));
    const std::vector<Event> later = pair.receive(std::chrono::milliseconds(1000));
    passed &= expect(later.size() == 1 && later[0].getOr<std::int64_t>("n", 0) == 3,
                     "later events still arrive normally");
    return passed;
}

bool leaveSendsEventsPublishedJustBefore()
{
    SessionPair pair;
    bool passed = expect(pair.open(), "both sessions connect");

    pair.sender->publishEvent(ping(9));
    pair.sender->leave();
    const std::vector<Event> received = pair.receive(std::chrono::milliseconds(1000));
    passed &= expect(received.size() == 1 && received[0].getOr<std::int64_t>("n", 0) == 9,
                     "an event published right before leave() still reaches the others");
    pair.sender.reset();
    passed &= expect(waitFor(
                         [&] {
                             pair.frame();
                             return pair.receiver->remotePlayers().empty();
                         },
                         std::chrono::milliseconds(1000)),
                     "the sender still leaves cleanly afterwards");
    return passed;
}

// The whole path a game uses: EventManager -> bridge -> Session -> host ->
// Session -> bridge -> EventManager, with a text and a binary session.
bool sessionsShareEventsThroughBridges()
{
    Network::NetworkServerHost host(serverConfig(), hostConfig());
    host.start();
    RealTimeClock realTime;

    struct Game {
        ManualClock clock;
        EventManager events{clock};
        std::unique_ptr<Multiplayer::Session> session;
        std::unique_ptr<NetworkEventBridge> bridge;
        std::vector<Event> pings;
    };
    Game games[2];
    for (int i = 0; i < 2; ++i) {
        Multiplayer::Config config;
        config.serverEndpoint = host.boundEndpoint();
        config.clientServerDataFormat = i == 0 ? Net::DataFormat::TextMultipart : Net::DataFormat::Binary;
        games[i].session = Multiplayer::Session::open(config, realTime);
        games[i].bridge = std::make_unique<NetworkEventBridge>(games[i].events, *games[i].session,
                                                               std::vector<std::string>{"Ping"});
        Game& game = games[i];
        game.events.subscribe("Ping", [&game](const Event& event) { game.pings.push_back(event); });
    }
    const auto frame = [&] {
        for (Game& game : games) {
            game.clock.advance(kGameTicsPerSecond / 60);
            game.session->update();
            game.bridge->apply();
            game.events.dispatch();
            game.session->publishLocalPlayer(0.0F, 0.0F);
        }
    };

    bool passed = expect(waitFor(
                             [&] {
                                 frame();
                                 return games[0].session->state() == Multiplayer::State::Ready &&
                                        games[1].session->state() == Multiplayer::State::Ready;
                             },
                             std::chrono::milliseconds(3000)),
                         "both sessions connect");

    games[0].events.raise(ping(1));
    games[1].events.raise(ping(2));
    passed &= expect(waitFor(
                         [&] {
                             frame();
                             return games[0].pings.size() == 2 && games[1].pings.size() == 2;
                         },
                         std::chrono::milliseconds(3000)),
                     "each game handles its own ping and the other's");
    for (int i = 0; i < 20; ++i) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (int i = 0; i < 2; ++i) {
        const std::vector<Event>& pings = games[i].pings;
        const Multiplayer::PlayerId other = games[1 - i].session->localPlayerId();
        passed &= expect(pings.size() == 2, "no ping echoes back or arrives twice");
        if (pings.size() != 2) {
            continue;
        }
        passed &= expect(pings[0].origin == 0 && pings[0].getOr<std::int64_t>("n", 0) == i + 1,
                         "a game handles its own ping first, locally");
        passed &= expect(pings[1].origin == other && pings[1].getOr<std::int64_t>("n", 0) == 2 - i &&
                             pings[1].has("sentAt"),
                         "the other game's ping arrives with its origin and sentAt");
    }
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= serverRelaysToEveryoneElseOnce();
    passed &= resentEventsAreRelayedOnce();
    passed &= acknowledgementsClearTheOutbox();
    passed &= repliesCarryAtMostSixtyFourEvents();
    passed &= fullOutboxDropsTheOldestAndCountsThem();
    passed &= aSkippedEventIsRefused();
    passed &= rejoiningKeepsTheOutboxAndSequences();
    passed &= bothFormatsCarryEventsAndAcks();
    passed &= decodersRejectBrokenEventFields();
    passed &= clientResendsUntilAcceptedAndHandlesEachEventOnce();
    passed &= clientKeepsOrRenumbersEventsOnRejoin();
    passed &= fullClientQueueDropsTheOldestUnsentEvents();
    passed &= clientIncomingQueueIsCapped();
    passed &= mixedClientsExchangeEventsThroughTheHost();
    passed &= bridgeSharesOnlyListedLocalEvents();
    passed &= bridgeRefusesWhatCannotWork();
    passed &= pacedSessionSendsEventsWithoutWaiting();
    passed &= sessionKeepsOnlyTheLatestFrameOfEvents();
    passed &= leaveSendsEventsPublishedJustBefore();
    passed &= sessionsShareEventsThroughBridges();

    if (!passed) {
        return 1;
    }
    std::cout << "Network event tests passed\n";
    return 0;
}
