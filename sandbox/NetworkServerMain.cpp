#include "NetworkServer.hpp"
#include "NetworkDemoConfig.hpp"
#include "TimeSource.hpp"

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

Network::Message receiveMessage(zmq::socket_t& socket)
{
    std::vector<zmq::message_t> raw;
    const auto received = zmq::recv_multipart(socket, std::back_inserter(raw));
    if (!received.has_value()) {
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

} // namespace

int main(int argc, char* argv[])
{
    const std::string endpoint = argc > 1 ? argv[1] : "tcp://*:5555";

    try {
        zmq::context_t context(1);
        zmq::socket_t socket(context, zmq::socket_type::rep);
        socket.set(zmq::sockopt::linger, 0);
        socket.bind(endpoint);

        RealTimeClock clock;
        Network::NetworkServer server(clock, NetworkDemo::makeServerConfig());
        std::cout << "Network server listening on " << endpoint << "\n";

        while (true) {
            sendMessage(socket, server.handle(receiveMessage(socket)));
        }
    } catch (const std::exception& exception) {
        std::cerr << "Network server error: " << exception.what() << '\n';
        return 1;
    }
}
