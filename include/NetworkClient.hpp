#pragma once

#include "NetworkProtocol.hpp"

#include <memory>
#include <string>

namespace Network {

// Non-blocking REQ client for a Game update loop. It sends intent only; the
// latest WorldSnapshot always comes from the authoritative server.
class NetworkClient {
public:
    explicit NetworkClient(std::string endpoint = "tcp://127.0.0.1:5555");
    ~NetworkClient();

    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;
    NetworkClient(NetworkClient&&) noexcept;
    NetworkClient& operator=(NetworkClient&&) noexcept;

    void start();
    void poll();
    void submitInput(int horizontal, int vertical);
    void leave();

    ConnectionState state() const;
    PlayerId playerId() const;
    const WorldSnapshot& snapshot() const;
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Network
