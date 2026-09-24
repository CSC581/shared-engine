#pragma once

#include "NetworkServer.hpp"
#include "TimeSource.hpp"
#include "ZmqMessage.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

// The shared-world authority, hosted inside a player's own process instead of
// a separate one — a listen-server.
//
// Section 5 allows the authority to be either dedicated or hosted by one of
// the players, and this is the hosted half. It is deliberately the same
// Network::NetworkServer the standalone `network-server` binary runs, driven
// by the same REP loop, so "dedicated or listen-server" is a question of which
// process the object lives in and nothing else. A peer that hosts gains no
// privileges in the peer mesh: player data still goes peer to peer, and
// hosting buys authority over the platforms alone.
//
// Sandbox code. The engine has no opinion about who hosts what.
class ListenServer {
public:
    ListenServer(std::string bindEndpoint, Network::ServerConfig config)
        : bindEndpoint_(std::move(bindEndpoint)), server_(clock_, std::move(config))
    {
    }

    ~ListenServer() { stop(); }

    ListenServer(const ListenServer&) = delete;
    ListenServer& operator=(const ListenServer&) = delete;

    // Throws zmq::error_t if the address is taken, which is how a second peer
    // launched with --host finds out that somebody is already hosting.
    void start()
    {
        zmq::socket_t socket(context_, zmq::socket_type::rep);
        socket.set(zmq::sockopt::linger, 0);
        // Wake periodically so the loop notices a request to stop.
        socket.set(zmq::sockopt::rcvtimeo, 200);
        socket.bind(bindEndpoint_);

        running_.store(true);
        // The socket moves onto the thread that will own it from here on;
        // creating the thread is the memory barrier ZeroMQ asks for.
        thread_ = std::thread([this, moved = std::move(socket)]() mutable { run(std::move(moved)); });
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
        // Platform motion is real time and has to keep running whether or not
        // a client happens to be asking, so it gets its own thread rather than
        // being driven by request arrival.
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
            if (!received) {
                continue;
            }

            Net::send(socket, server_.handle(request));
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
