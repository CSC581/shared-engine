#include "NetworkClient.hpp"

#include "TimeSource.hpp"
#include "TimeUnits.hpp"

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <chrono>
#include <algorithm>
#include <utility>
#include <vector>

namespace Network {
namespace {

constexpr std::int64_t retryDelayNs = kNsPerSec;

std::vector<zmq::const_buffer> makeBuffers(const Message& message)
{
    std::vector<zmq::const_buffer> buffers;
    buffers.reserve(message.size());
    for (const std::string& field : message) {
        buffers.push_back(zmq::buffer(field));
    }
    return buffers;
}

bool sendMessage(zmq::socket_t& socket, const Message& message)
{
    const std::vector<zmq::const_buffer> buffers = makeBuffers(message);
    return zmq::send_multipart(socket, buffers, zmq::send_flags::dontwait).has_value();
}

Message receiveMessage(zmq::socket_t& socket, bool& received)
{
    std::vector<zmq::message_t> raw;
    const auto result = zmq::recv_multipart(socket, std::back_inserter(raw), zmq::recv_flags::dontwait);
    received = result.has_value();
    if (!received) {
        return {};
    }

    Message message;
    message.reserve(raw.size());
    for (const zmq::message_t& field : raw) {
        message.push_back(field.to_string());
    }
    return message;
}

} // namespace

struct NetworkClient::Impl {
    explicit Impl(std::string endpointValue)
        : endpoint(std::move(endpointValue))
    {
    }

    void reconnect()
    {
        socket.reset();
        socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::req);
        socket->set(zmq::sockopt::linger, 0);
        socket->connect(endpoint);
        waitingForReply = false;
        state = ConnectionState::Connecting;
        sendJoin();
    }

    void sendJoin()
    {
        if (!socket || !sendMessage(*socket, encodeJoin())) {
            scheduleRetry("could not send JOIN request");
            return;
        }

        waitingForReply = true;
        requestSentAt = clock.now();
    }

    void scheduleRetry(const std::string& message)
    {
        waitingForReply = false;
        state = ConnectionState::Connecting;
        error = message;
        nextRetryAt = clock.now() + retryDelayNs;
    }

    zmq::context_t context{1};
    std::unique_ptr<zmq::socket_t> socket;
    RealTimeClock clock;
    std::string endpoint;
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
    if (impl_->state == ConnectionState::Disconnected) {
        return;
    }

    if (!impl_->waitingForReply) {
        if (impl_->state == ConnectionState::Connecting && now >= impl_->nextRetryAt) {
            impl_->reconnect();
        }
        return;
    }

    bool received = false;
    const Message message = receiveMessage(*impl_->socket, received);
    if (!received) {
        if (now - impl_->requestSentAt >= retryDelayNs) {
            impl_->scheduleRetry("waiting for server at " + impl_->endpoint);
        }
        return;
    }

    impl_->waitingForReply = false;
    Reply reply;
    std::string error;
    if (!decodeReply(message, reply, error)) {
        impl_->state = ConnectionState::Error;
        impl_->error = error;
        return;
    }

    if (reply.type == ReplyType::Error) {
        impl_->state = ConnectionState::Error;
        impl_->error = reply.error;
        return;
    }

    if (reply.type == ReplyType::Goodbye) {
        impl_->state = ConnectionState::Disconnected;
        return;
    }

    if (reply.type == ReplyType::Welcome) {
        impl_->playerId = reply.playerId;
        impl_->state = ConnectionState::Connected;
        impl_->error.clear();
    }

    impl_->snapshot = std::move(reply.snapshot);
}

void NetworkClient::submitInput(int horizontal, int vertical)
{
    if (impl_->state != ConnectionState::Connected || impl_->waitingForReply) {
        return;
    }

    MovementInput input;
    input.horizontal = std::clamp(horizontal, -1, 1);
    input.vertical = std::clamp(vertical, -1, 1);
    input.sequence = impl_->nextSequence++;
    if (!sendMessage(*impl_->socket, encodeInput(impl_->playerId, input))) {
        impl_->scheduleRetry("could not send movement input");
        return;
    }

    impl_->waitingForReply = true;
    impl_->requestSentAt = impl_->clock.now();
}

void NetworkClient::leave()
{
    if (impl_->state == ConnectionState::Connected && !impl_->waitingForReply) {
        sendMessage(*impl_->socket, encodeLeave(impl_->playerId));
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
