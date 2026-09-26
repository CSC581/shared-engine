#pragma once

#include "ApexNetworkConfig.hpp"
#include "NetworkServer.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

// Apex's shared-world authority hosted inside one player's process.
// Player state still travels through Multiplayer's peer mesh; this server
// supplies only the tower's authority-owned moving platform snapshot.
class ApexListenServer {
public:
    explicit ApexListenServer(std::string bindEndpoint)
        : bindEndpoint_(std::move(bindEndpoint)),
          server_(clock_, ApexNetwork::makeServerConfig())
    {
    }

    ~ApexListenServer() { stop(); }

    ApexListenServer(const ApexListenServer&) = delete;
    ApexListenServer& operator=(const ApexListenServer&) = delete;

    void start()
    {
        zmq::socket_t socket(context_, zmq::socket_type::rep);
        socket.set(zmq::sockopt::linger, 0);
        socket.set(zmq::sockopt::rcvtimeo, 200);
        socket.bind(bindEndpoint_);

        running_.store(true);
        thread_ = std::thread([this, moved = std::move(socket)]() mutable {
            run(std::move(moved));
        });
    }

    void stop()
    {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run(zmq::socket_t socket)
    {
        std::thread platforms([this] {
            while (running_.load()) {
                server_.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });

        while (running_.load()) {
            bool received = false;
            Network::Message request;
            try {
                request = Net::receive(socket, received);
            } catch (const zmq::error_t&) {
                continue;
            }
            if (received) {
                Net::send(socket, server_.handle(request));
            }
        }

        platforms.join();
    }

    std::string bindEndpoint_;
    zmq::context_t context_{1};
    RealTimeClock clock_;
    Network::NetworkServer server_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
