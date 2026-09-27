#include "WorldStateClient.hpp"

#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "ZmqMessage.hpp"

#include <utility>

namespace Network {
namespace {

constexpr std::int64_t retryDelayNs = kNsPerSec;
constexpr std::int64_t pollIntervalNs = 50 * 1000000;

} // namespace

struct WorldStateClient::Impl {
    explicit Impl(std::string endpointValue, const TimeSource* timeSource = nullptr)
        : clock(timeSource ? *timeSource : realClock), endpoint(std::move(endpointValue))
    {
    }

    void fail(std::string message)
    {
        socket.reset();
        waitingForReply = false;
        state = ConnectionState::Error;
        error = std::move(message);
        snapshot = {};
    }

    void scheduleRetry(std::string message)
    {
        socket.reset();
        waitingForReply = false;
        state = ConnectionState::Connecting;
        error = std::move(message);
        snapshot = {};
        nextRequestAt = clock.now() + retryDelayNs;
    }

    void sendRequest()
    {
        if (!socket || !Net::send(*socket, encodeGetWorld(), zmq::send_flags::dontwait)) {
            scheduleRetry("could not request shared world state");
            return;
        }
        waitingForReply = true;
        requestSentAt = clock.now();
    }

    void reconnect()
    {
        try {
            socket.reset();
            socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::req);
            socket->set(zmq::sockopt::linger, 0);
            socket->connect(endpoint);
        } catch (const zmq::error_t& exception) {
            fail(std::string("Cannot connect to world authority: ") + exception.what());
            return;
        }
        state = ConnectionState::Connecting;
        error.clear();
        sendRequest();
    }

    zmq::context_t context{1};
    std::unique_ptr<zmq::socket_t> socket;
    RealTimeClock realClock;
    const TimeSource& clock;
    std::string endpoint;
    ConnectionState state = ConnectionState::Disconnected;
    WorldStateSnapshot snapshot;
    std::string error;
    std::int64_t requestSentAt = 0;
    std::int64_t nextRequestAt = 0;
    bool waitingForReply = false;
};

WorldStateClient::WorldStateClient(std::string endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint)))
{
}

WorldStateClient::WorldStateClient(const TimeSource& clock, std::string endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint), &clock))
{
}

WorldStateClient::~WorldStateClient() = default;
WorldStateClient::WorldStateClient(WorldStateClient&&) noexcept = default;
WorldStateClient& WorldStateClient::operator=(WorldStateClient&&) noexcept = default;

void WorldStateClient::start()
{
    if (impl_->state == ConnectionState::Disconnected || impl_->state == ConnectionState::Error) {
        impl_->reconnect();
    }
}

void WorldStateClient::poll()
{
    const std::int64_t now = impl_->clock.now();
    if (impl_->state == ConnectionState::Disconnected || impl_->state == ConnectionState::Error) {
        return;
    }

    if (!impl_->waitingForReply) {
        if (now >= impl_->nextRequestAt) {
            if (impl_->state == ConnectionState::Connecting) {
                impl_->reconnect();
            } else {
                impl_->sendRequest();
            }
        }
        return;
    }

    bool received = false;
    Message message;
    try {
        message = Net::receive(*impl_->socket, received, zmq::recv_flags::dontwait);
    } catch (const zmq::error_t& exception) {
        impl_->scheduleRetry(std::string("Could not receive world state: ") + exception.what());
        return;
    }
    if (!received) {
        if (now - impl_->requestSentAt >= retryDelayNs) {
            impl_->scheduleRetry("waiting for world authority at " + impl_->endpoint);
        }
        return;
    }

    impl_->waitingForReply = false;
    Reply reply;
    std::string error;
    if (!decodeReply(message, reply, error)) {
        impl_->fail(error == "unsupported protocol version"
                        ? "Protocol version mismatch. Rebuild and restart both server and clients."
                        : "Invalid world authority reply: " + error);
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
    if (reply.type != ReplyType::WorldState) {
        impl_->fail("World authority returned a player reply instead of shared world state");
        return;
    }

    impl_->snapshot = std::move(reply.worldState);
    impl_->state = ConnectionState::Connected;
    impl_->error.clear();
    impl_->nextRequestAt = now + pollIntervalNs;
}

void WorldStateClient::stop()
{
    impl_->socket.reset();
    impl_->waitingForReply = false;
    impl_->state = ConnectionState::Disconnected;
    impl_->snapshot = {};
    impl_->error.clear();
}

ConnectionState WorldStateClient::state() const
{
    return impl_->state;
}

const WorldStateSnapshot& WorldStateClient::snapshot() const
{
    return impl_->snapshot;
}

const std::string& WorldStateClient::error() const
{
    return impl_->error;
}

} // namespace Network
