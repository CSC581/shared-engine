#include "NetworkClient.hpp"

#include "BinaryNetworkProtocol.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "ZmqMessage.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Network {
namespace {

constexpr std::int64_t retryDelayNs = kNsPerSec;

Message encodeTransportRequest(const Request& request, Net::DataFormat format,
                               Net::SnapshotStrategy strategy, std::uint64_t acknowledgedTick = 0,
                               bool forceFull = false)
{
    if (format == Net::DataFormat::Binary) {
        if (strategy == Net::SnapshotStrategy::PropertyDelta &&
            (request.type == RequestType::Join || request.type == RequestType::Position)) {
            BinaryRequestEnvelope envelope;
            envelope.request = request;
            envelope.snapshotStrategy = strategy;
            envelope.acknowledgedServerTick = acknowledgedTick;
            envelope.forceFullSnapshot = forceFull;
            return {encodeBinaryRequest(envelope)};
        }
        return {encodeBinaryRequest(request)};
    }
    switch (request.type) {
    case RequestType::Join:
        return encodeJoin(request.sessionToken, request.playerName);
    case RequestType::Position:
        return encodePosition(request.playerId, request.sessionToken, request.position);
    case RequestType::Leave:
        return encodeLeave(request.playerId, request.sessionToken);
    case RequestType::GetWorld:
        return encodeGetWorld();
    }
    return {};
}

bool decodeTransportReply(const Message& message, Net::DataFormat format,
                          BinaryReplyEnvelope& envelope, std::string& error)
{
    if (format == Net::DataFormat::Binary) {
        if (message.size() != 1) {
            error = "binary reply must contain exactly one frame";
            return false;
        }
        return decodeBinaryReply(message.front(), envelope, error);
    }
    envelope = {};
    return decodeReply(message, envelope.reply, error);
}

std::uint64_t payloadBytes(const Message& message)
{
    std::uint64_t bytes = 0;
    for (const std::string& frame : message) {
        bytes += frame.size();
    }
    return bytes;
}

SessionToken makeSessionToken()
{
    std::random_device random;
    std::ostringstream token;
    token.imbue(std::locale::classic());
    token << std::hex << std::setfill('0');
    for (int part = 0; part < 4; ++part) {
        token << std::setw(8) << static_cast<std::uint32_t>(random());
    }
    return token.str();
}

} // namespace

struct NetworkClient::Impl {
    explicit Impl(std::string endpointValue, Net::DataFormat formatValue,
                  Net::SnapshotStrategy strategyValue,
                  const TimeSource* timeSource = nullptr)
        : clock(timeSource ? *timeSource : realClock),
          handshakeEndpoint(std::move(endpointValue)),
          format(formatValue), strategy(strategyValue)
    {
        if (strategy == Net::SnapshotStrategy::PropertyDelta && format != Net::DataFormat::Binary) {
            throw std::invalid_argument("property deltas require binary client-server format");
        }
    }

    void reconnect()
    {
        try {
            socket.reset();
            socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::req);
            socket->set(zmq::sockopt::linger, 0);
            socket->connect(handshakeEndpoint);
        } catch (const zmq::error_t& exception) {
            fail(std::string("Cannot connect: ") + exception.what() + ". Check the server address.");
            return;
        }
        sessionEndpoint.clear();
        waitingForReply = false;
        forceFullSnapshot = false;
        explicitFullSnapshotRequested = false;
        state = ConnectionState::Connecting;
        error.clear();
        sendJoin();
    }

    // Move the REQ socket onto the private per-client worker address. The JOIN
    // already succeeded on the handshake socket; do not send another.
    bool connectSession(const std::string& endpoint)
    {
        try {
            socket.reset();
            socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::req);
            socket->set(zmq::sockopt::linger, 0);
            socket->connect(endpoint);
        } catch (const zmq::error_t& exception) {
            scheduleRetry(std::string("Cannot connect to session endpoint: ") + exception.what());
            return false;
        }
        sessionEndpoint = endpoint;
        waitingForReply = false;
        return true;
    }

    void fail(const std::string& message)
    {
        socket.reset();
        waitingForReply = false;
        state = ConnectionState::Error;
        error = message;
        playerId = 0;
        snapshot = {};
        sessionEndpoint.clear();
        forceFullSnapshot = false;
        explicitFullSnapshotRequested = false;
    }

    void sendJoin()
    {
        Request request;
        request.type = RequestType::Join;
        request.sessionToken = sessionToken;
        request.playerName = playerName;
        Message message;
        try {
            message = encodeTransportRequest(request, format, strategy);
        } catch (const std::invalid_argument& exception) {
            fail(exception.what());
            return;
        }
        if (!socket || !Net::send(*socket, message, zmq::send_flags::dontwait)) {
            scheduleRetry("could not send JOIN request");
            return;
        }

        recordSent(message);

        waitingForReply = true;
        requestSentAt = clock.now();
    }

    void scheduleRetry(const std::string& message)
    {
        socket.reset();
        waitingForReply = false;
        state = ConnectionState::Connecting;
        error = message;
        // A disconnected world is no longer authoritative. The next WELCOME
        // replaces it after this client has joined the server again.
        playerId = 0;
        snapshot = {};
        forceFullSnapshot = false;
        explicitFullSnapshotRequested = false;
        sessionEndpoint.clear();
        nextRetryAt = clock.now() + retryDelayNs;
    }

    void recordSent(const Message& message)
    {
        ++transport.messagesSent;
        transport.framesSent += message.size();
        transport.payloadBytesSent += payloadBytes(message);
    }

    void recordReceived(const Message& message)
    {
        ++transport.messagesReceived;
        transport.framesReceived += message.size();
        transport.payloadBytesReceived += payloadBytes(message);
    }

    zmq::context_t context{1};
    std::unique_ptr<zmq::socket_t> socket;
    RealTimeClock realClock;
    const TimeSource& clock;
    // Well-known handshake address from the constructor.
    std::string handshakeEndpoint;
    Net::DataFormat format = Net::DataFormat::TextMultipart;
    Net::SnapshotStrategy strategy = Net::SnapshotStrategy::FullSnapshot;
    // Private worker address after WELCOME; empty until assigned.
    std::string sessionEndpoint;
    SessionToken sessionToken = makeSessionToken();
    std::string playerName;
    ConnectionState state = ConnectionState::Disconnected;
    PlayerId playerId = 0;
    WorldSnapshot snapshot;
    std::string error;
    std::uint64_t nextSequence = 1;
    std::int64_t requestSentAt = 0;
    std::int64_t nextRetryAt = 0;
    bool waitingForReply = false;
    bool forceFullSnapshot = false;
    bool explicitFullSnapshotRequested = false;
    TransportTelemetry transport;
    SnapshotTelemetry snapshotStats;
};

NetworkClient::NetworkClient(std::string endpoint, Net::DataFormat format,
                             Net::SnapshotStrategy strategy)
    : impl_(std::make_unique<Impl>(std::move(endpoint), format, strategy))
{
}

NetworkClient::NetworkClient(const TimeSource& clock, std::string endpoint, Net::DataFormat format,
                             Net::SnapshotStrategy strategy)
    : impl_(std::make_unique<Impl>(std::move(endpoint), format, strategy, &clock))
{
}

NetworkClient::~NetworkClient() = default;
NetworkClient::NetworkClient(NetworkClient&&) noexcept = default;
NetworkClient& NetworkClient::operator=(NetworkClient&&) noexcept = default;

void NetworkClient::start()
{
    if (impl_->state == ConnectionState::Disconnected || impl_->state == ConnectionState::Error) {
        impl_->reconnect();
    }
}

void NetworkClient::poll()
{
    const std::int64_t now = impl_->clock.now();
    if (impl_->state == ConnectionState::Disconnected || impl_->state == ConnectionState::Error) {
        return;
    }

    if (!impl_->waitingForReply) {
        if (impl_->state == ConnectionState::Connecting && now >= impl_->nextRetryAt) {
            impl_->reconnect();
        }
        return;
    }

    bool received = false;
    Message message;
    try {
        message = Net::receive(*impl_->socket, received, zmq::recv_flags::dontwait);
    } catch (const zmq::error_t& exception) {
        impl_->scheduleRetry(std::string("Could not receive server reply: ") + exception.what());
        return;
    }
    if (!received) {
        if (now - impl_->requestSentAt >= retryDelayNs) {
            impl_->scheduleRetry("waiting for server at " + impl_->handshakeEndpoint);
        }
        return;
    }

    impl_->recordReceived(message);
    impl_->waitingForReply = false;
    BinaryReplyEnvelope envelope;
    std::string error;
    if (!decodeTransportReply(message, impl_->format, envelope, error)) {
        impl_->fail(error == "unsupported protocol version"
                        ? "Protocol version mismatch. Rebuild and restart both server and clients."
                        : "Invalid server reply: " + error);
        return;
    }
    Reply& reply = envelope.reply;

    if (envelope.kind == BinaryReplyKind::Standard && reply.type == ReplyType::Error) {
        if (reply.error == "unsupported protocol version") {
            impl_->fail("Protocol version mismatch. Rebuild and restart both server and clients.");
        } else {
            impl_->scheduleRetry(reply.error);
        }
        return;
    }

    if (envelope.kind == BinaryReplyKind::Standard && reply.type == ReplyType::Goodbye) {
        impl_->state = ConnectionState::Disconnected;
        return;
    }

    if (reply.type == ReplyType::Welcome) {
        if (impl_->state != ConnectionState::Connecting) {
            impl_->fail("Unexpected WELCOME reply while already connected");
            return;
        }
        const bool deltaWelcome = envelope.kind == BinaryReplyKind::DeltaWelcome;
        if (deltaWelcome != (impl_->strategy == Net::SnapshotStrategy::PropertyDelta)) {
            impl_->fail("Server did not accept the requested snapshot strategy");
            return;
        }
        impl_->playerId = reply.playerId;
        impl_->error.clear();
        if (!reply.sessionEndpoint.empty() && !impl_->connectSession(reply.sessionEndpoint)) {
            return;
        }
        impl_->state = ConnectionState::Connected;
        impl_->snapshot = std::move(reply.snapshot);
        ++impl_->snapshotStats.fullSnapshotsReceived;
        return;
    }

    if (impl_->state != ConnectionState::Connected) {
        impl_->fail("Unexpected reply to player request");
        return;
    }
    if (envelope.kind == BinaryReplyKind::SnapshotDelta) {
        if (impl_->strategy != Net::SnapshotStrategy::PropertyDelta) {
            impl_->fail("Unexpected property-delta reply");
            return;
        }
        WorldSnapshot next;
        if (!applySnapshotDelta(impl_->snapshot, envelope.delta, next, error)) {
            ++impl_->snapshotStats.baselineMismatches;
            impl_->forceFullSnapshot = true;
            impl_->error = "Snapshot delta rejected; requesting full snapshot: " + error;
            return;
        }
        impl_->snapshotStats.playerAdds += envelope.delta.addedPlayers.size();
        impl_->snapshotStats.playerRemoves += envelope.delta.removedPlayers.size();
        impl_->snapshotStats.playerPatches += envelope.delta.changedPlayers.size();
        impl_->snapshotStats.platformAdds += envelope.delta.addedPlatforms.size();
        impl_->snapshotStats.platformRemoves += envelope.delta.removedPlatforms.size();
        impl_->snapshotStats.platformPatches += envelope.delta.changedPlatforms.size();
        ++impl_->snapshotStats.deltaSnapshotsReceived;
        impl_->snapshot = std::move(next);
        impl_->forceFullSnapshot = false;
        impl_->error.clear();
        return;
    }
    if (envelope.kind != BinaryReplyKind::Standard || reply.type != ReplyType::Snapshot) {
        impl_->fail("Unexpected reply to player request");
        return;
    }
    impl_->snapshot = std::move(reply.snapshot);
    impl_->forceFullSnapshot = false;
    impl_->explicitFullSnapshotRequested = false;
    ++impl_->snapshotStats.fullSnapshotsReceived;
    impl_->error.clear();
}

void NetworkClient::setPlayerName(std::string name)
{
    impl_->playerName = std::move(name);
}

void NetworkClient::submitPosition(float x, float y, const std::string& data)
{
    if (impl_->state != ConnectionState::Connected || impl_->waitingForReply) {
        return;
    }

    if (!std::isfinite(x) || !std::isfinite(y)) {
        impl_->error = "position must be finite";
        return;
    }

    const PositionUpdate position{x, y, impl_->nextSequence++, data};
    Request request;
    request.type = RequestType::Position;
    request.playerId = impl_->playerId;
    request.sessionToken = impl_->sessionToken;
    request.position = position;
    Message message;
    const bool requestFull = impl_->forceFullSnapshot || impl_->explicitFullSnapshotRequested;
    try {
        message = encodeTransportRequest(request, impl_->format, impl_->strategy,
                                         impl_->snapshot.serverTick,
                                         requestFull);
    } catch (const std::invalid_argument& exception) {
        impl_->error = exception.what();
        return;
    }
    if (!Net::send(*impl_->socket, message, zmq::send_flags::dontwait)) {
        impl_->scheduleRetry("could not send position update");
        return;
    }

    impl_->recordSent(message);
    if (requestFull) ++impl_->snapshotStats.resyncRequestsSent;
    impl_->error.clear();
    impl_->waitingForReply = true;
    impl_->requestSentAt = impl_->clock.now();
}

void NetworkClient::requestFullSnapshot()
{
    if (impl_->strategy == Net::SnapshotStrategy::PropertyDelta) {
        impl_->explicitFullSnapshotRequested = true;
    }
}

void NetworkClient::leave()
{
    if (impl_->state == ConnectionState::Connected && !impl_->waitingForReply) {
        Request request;
        request.type = RequestType::Leave;
        request.playerId = impl_->playerId;
        request.sessionToken = impl_->sessionToken;
        try {
            const Message message = encodeTransportRequest(request, impl_->format, impl_->strategy);
            if (Net::send(*impl_->socket, message, zmq::send_flags::dontwait)) {
                impl_->recordSent(message);
            }
        } catch (const std::invalid_argument&) {
            // The fields were already accepted for JOIN, so this can only be
            // an internal invariant failure. Teardown must still complete.
        }
    }
    impl_->socket.reset();
    impl_->state = ConnectionState::Disconnected;
}

ConnectionState NetworkClient::state() const
{
    return impl_->state;
}

bool NetworkClient::awaitingReply() const
{
    return impl_->waitingForReply;
}

PlayerId NetworkClient::playerId() const
{
    return impl_->playerId;
}

const WorldSnapshot& NetworkClient::snapshot() const
{
    return impl_->snapshot;
}

const std::string& NetworkClient::error() const
{
    return impl_->error;
}

TransportTelemetry NetworkClient::telemetry() const
{
    return impl_->transport;
}

SnapshotTelemetry NetworkClient::snapshotTelemetry() const
{
    return impl_->snapshotStats;
}

} // namespace Network
