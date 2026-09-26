#pragma once

#include "NetworkProtocol.hpp"

#include <memory>
#include <string>

class TimeSource;

namespace Network {

// Non-blocking read-only client for server-authored shared objects. Unlike
// NetworkClient, this never joins the player roster or submits a player pose.
class WorldStateClient {
public:
    explicit WorldStateClient(std::string endpoint = "tcp://127.0.0.1:5555");
    // The monotonic nanosecond clock must outlive this client.
    WorldStateClient(const TimeSource& clock, std::string endpoint = "tcp://127.0.0.1:5555");
    ~WorldStateClient();

    WorldStateClient(const WorldStateClient&) = delete;
    WorldStateClient& operator=(const WorldStateClient&) = delete;
    WorldStateClient(WorldStateClient&&) noexcept;
    WorldStateClient& operator=(WorldStateClient&&) noexcept;

    void start();
    void poll();
    void stop();

    ConnectionState state() const;
    const WorldStateSnapshot& snapshot() const;
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Network
