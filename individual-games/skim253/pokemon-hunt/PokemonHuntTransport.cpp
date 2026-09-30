#include "PokemonHuntTransport.hpp"

#include "ZmqMessage.hpp"

#include <stdexcept>

namespace PokemonHunt {

struct Transport::Impl {
    zmq::context_t context{1};
};

Transport::Transport()
    : impl_(std::make_unique<Impl>())
{
}

Transport::~Transport() = default;

void Transport::shutdown() noexcept
{
    try {
        impl_->context.shutdown();
    } catch (const zmq::error_t&) {
        // Already shut down or closing.
    }
}

struct ReplySocket::Impl {
    explicit Impl(zmq::context_t& context)
        : socket(context, zmq::socket_type::rep)
    {
    }

    zmq::socket_t socket;
};

ReplySocket::ReplySocket(Transport& transport, std::chrono::milliseconds receiveTimeout)
    : impl_(std::make_unique<Impl>(transport.impl_->context))
{
    impl_->socket.set(zmq::sockopt::linger, 0);
    impl_->socket.set(zmq::sockopt::rcvtimeo, static_cast<int>(receiveTimeout.count()));
}

ReplySocket::~ReplySocket() = default;

std::string ReplySocket::bind(const std::string& endpoint)
{
    try {
        impl_->socket.bind(endpoint);
        return impl_->socket.get(zmq::sockopt::last_endpoint);
    } catch (const zmq::error_t& error) {
        throw std::runtime_error("could not bind " + endpoint + ": " + error.what());
    }
}

// Framing goes through the engine's Net::send and Net::receive, the same
// functions every engine socket uses.
bool ReplySocket::receive(Network::Message& message)
{
    bool received = false;
    try {
        message = Net::receive(impl_->socket, received);
    } catch (const zmq::error_t&) {
        return false;
    }
    return received;
}

bool ReplySocket::send(const Network::Message& message)
{
    return Net::send(impl_->socket, message);
}

} // namespace PokemonHunt
