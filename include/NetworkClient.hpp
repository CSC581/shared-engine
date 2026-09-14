#pragma once

#include "NetworkProtocol.hpp"

#include <memory>
#include <string>

class TimeSource;

namespace Network {

// Non-blocking REQ client. Games simulate their own character and submit its
// position; snapshots contain the latest positions stored by the server.
class NetworkClient {
public:
    explicit NetworkClient(std::string endpoint = "tcp://127.0.0.1:5555");
    // The clock must count monotonic nanoseconds and outlive this client.
    // Use unpaused real time in games so connection recovery keeps running.
    NetworkClient(const TimeSource& clock, std::string endpoint = "tcp://127.0.0.1:5555");
    ~NetworkClient();

    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;
    NetworkClient(NetworkClient&&) noexcept;
    NetworkClient& operator=(NetworkClient&&) noexcept;

    // Starts connecting, or explicitly retries after a terminal Error.
    void start();
    void poll();
    // Call every frame, even while stationary, to refresh presence and snapshots.
    // While awaiting a reply, submissions are skipped; no stale positions queue up.
    void submitPosition(float x, float y);
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
