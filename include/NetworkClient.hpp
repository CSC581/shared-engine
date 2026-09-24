#pragma once

#include "NetworkProtocol.hpp"

#include <memory>
#include <string>

class TimeSource;

namespace Network {

// Non-blocking REQ client. Games simulate their own character and submit its
// position; snapshots contain the latest positions stored by the server.
//
// The constructor endpoint is the handshake address. After WELCOME, if the
// server supplies a sessionEndpoint the client reconnects there for POSITION
// and LEAVE (Section 4 per-client worker). An empty sessionEndpoint means the
// handshake socket is also the session socket (single-REP tests / demos).
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

    // The name other players see. Sent once, at JOIN, so it must be set
    // before start(); changing it afterwards takes effect on the next rejoin.
    void setPlayerName(std::string name);

    // Starts connecting, or explicitly retries after a terminal Error.
    void start();
    void poll();
    // Call every frame, even while stationary, to refresh presence and snapshots.
    // While awaiting a reply, submissions are skipped; no stale positions queue up.
    //
    // `data` is whatever this game needs to say about its player beyond where
    // it is. The networking module never looks inside it: it carries it, the
    // server stores it, and every other client reads it back in
    // WorldSnapshot::players. Format and meaning belong to the game.
    void submitPosition(float x, float y, const std::string& data = {});
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
