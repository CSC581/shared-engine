#include "NetworkClient.hpp"

#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "ZmqMessage.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <random>
#include <sstream>
#include <utility>

namespace Network {
namespace {

constexpr std::int64_t retryDelayNs = kNsPerSec;

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
    explicit Impl(std::string endpointValue, const TimeSource* timeSource = nullptr)
        : clock(timeSource ? *timeSource : realClock),
          handshakeEndpoint(std::move(endpointValue))
    {
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
    }

    void sendJoin()
    {
        if (!socket || !Net::send(*socket, encodeJoin(sessionToken, playerName), zmq::send_flags::dontwait)) {
            scheduleRetry("could not send JOIN request");
            return;
        }

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
        sessionEndpoint.clear();
        nextRetryAt = clock.now() + retryDelayNs;
    }

    zmq::context_t context{1};
    std::unique_ptr<zmq::socket_t> socket;
    RealTimeClock realClock;
    const TimeSource& clock;
    // Well-known handshake address from the constructor.
    std::string handshakeEndpoint;
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
};

NetworkClient::NetworkClient(std::string endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint)))
{
}

NetworkClient::NetworkClient(const TimeSource& clock, std::string endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint), &clock))
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

    impl_->waitingForReply = false;
    Reply reply;
    std::string error;
    if (!decodeReply(message, reply, error)) {
        impl_->fail(error == "unsupported protocol version"
                        ? "Protocol version mismatch. Rebuild and restart both server and clients."
                        : "Invalid server reply: " + error);
        return;
    }

    if (reply.type == ReplyType::Error) {
        if (reply.error == "unsupported protocol version") {
            impl_->fail("Protocol version mismatch. Rebuild and restart both server and clients.");
        } else {
            impl_->scheduleRetry(reply.error);
        }
        return;
    }

    if (reply.type == ReplyType::Goodbye) {
        impl_->state = ConnectionState::Disconnected;
        return;
    }

    if (reply.type == ReplyType::Welcome) {
        if (impl_->state != ConnectionState::Connecting) {
            impl_->fail("Unexpected WELCOME reply while already connected");
            return;
        }
        impl_->playerId = reply.playerId;
        impl_->error.clear();
        if (!reply.sessionEndpoint.empty() && !impl_->connectSession(reply.sessionEndpoint)) {
            return;
        }
        impl_->state = ConnectionState::Connected;
    } else if (reply.type != ReplyType::Snapshot || impl_->state != ConnectionState::Connected) {
        impl_->fail("Unexpected reply to player request");
        return;
    }

    impl_->snapshot = std::move(reply.snapshot);
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
    if (!Net::send(*impl_->socket, encodePosition(impl_->playerId, impl_->sessionToken, position),
                   zmq::send_flags::dontwait)) {
        impl_->scheduleRetry("could not send position update");
        return;
    }

    impl_->error.clear();
    impl_->waitingForReply = true;
    impl_->requestSentAt = impl_->clock.now();
}

void NetworkClient::leave()
{
    if (impl_->state == ConnectionState::Connected && !impl_->waitingForReply) {
        Net::send(*impl_->socket, encodeLeave(impl_->playerId, impl_->sessionToken),
                  zmq::send_flags::dontwait);
    }
    impl_->socket.reset();
    impl_->state = ConnectionState::Disconnected;
}

ConnectionState NetworkClient::state() const
{
    return impl_->state;
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

} // namespace Network
