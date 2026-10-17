#pragma once

#include "NetworkDataFormat.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkSnapshotStrategy.hpp"

#include <cstdint>
#include <memory>
#include <string>

class TimeSource;

namespace Network {

struct TransportTelemetry {
    std::uint64_t messagesSent = 0;
    std::uint64_t messagesReceived = 0;
    std::uint64_t framesSent = 0;
    std::uint64_t framesReceived = 0;
    // Sum of application payload bytes in the ZeroMQ frames. Transport
    // framing and TCP/IP overhead are deliberately not estimated here.
    std::uint64_t payloadBytesSent = 0;
    std::uint64_t payloadBytesReceived = 0;
};

struct SnapshotTelemetry {
    std::uint64_t fullSnapshotsReceived = 0;
    std::uint64_t deltaSnapshotsReceived = 0;
    std::uint64_t resyncRequestsSent = 0;
    std::uint64_t baselineMismatches = 0;
    std::uint64_t playerAdds = 0;
    std::uint64_t playerRemoves = 0;
    std::uint64_t playerPatches = 0;
    std::uint64_t platformAdds = 0;
    std::uint64_t platformRemoves = 0;
    std::uint64_t platformPatches = 0;
};

// Non-blocking REQ client. Games simulate their own character and submit its
// position; snapshots contain the latest positions stored by the server.
//
// The constructor endpoint is the handshake address. After WELCOME, if the
// server supplies a sessionEndpoint the client reconnects there for POSITION
// and LEAVE (Section 4 per-client worker). An empty sessionEndpoint means the
// handshake socket is also the session socket (single-REP tests / demos).
class NetworkClient {
public:
    explicit NetworkClient(std::string endpoint = "tcp://127.0.0.1:5555",
                           Net::DataFormat format = Net::DataFormat::TextMultipart,
                           Net::SnapshotStrategy strategy = Net::SnapshotStrategy::FullSnapshot);
    // The clock must count monotonic nanoseconds and outlive this client.
    // Use unpaused real time in games so connection recovery keeps running.
    NetworkClient(const TimeSource& clock, std::string endpoint = "tcp://127.0.0.1:5555",
                  Net::DataFormat format = Net::DataFormat::TextMultipart,
                  Net::SnapshotStrategy strategy = Net::SnapshotStrategy::FullSnapshot);
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
    // A request is out and its reply has not been read yet. REQ/REP allows one
    // request in flight, so nothing else (a position, a LEAVE) can be sent
    // until poll() reads it.
    bool awaitingReply() const;
    PlayerId playerId() const;
    const WorldSnapshot& snapshot() const;
    const std::string& error() const;
    // Cumulative successful sends and completed receives over this client's
    // lifetime, including JOIN/LEAVE and reconnect traffic.
    TransportTelemetry telemetry() const;
    SnapshotTelemetry snapshotTelemetry() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Network
