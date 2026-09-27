#pragma once

#include "NetworkServer.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Runs a NetworkServer on the network: the sockets, threads and shutdown that
// turn the session/state store into something clients can reach.
//
// NetworkServer deliberately knows nothing about ZeroMQ, so that it can be
// unit-tested by handing it messages directly. Something still has to bind a
// socket, route requests to it, give each client its own thread and tear all
// of that down cleanly, and that is the same job for every game's server. This
// class is that job, written once, so a server executable (dedicated or
// hosted inside a player's process) is its ServerConfig, its command line and
// its signal handling, and nothing else.
//
// The header does not include ZeroMQ: code that hosts a server never needs to
// see the transport.
namespace Network {

enum class HostMode {
    // Section 4: a handshake socket that accepts JOIN, plus one REP worker
    // thread per client on its own port. A slow client blocks only its own
    // worker; everybody else keeps being answered. Plain REQ/REP throughout —
    // no Router/Dealer.
    Dedicated,
    // One REP socket answering every request itself. Right for a listen-server
    // that only hands out shared world state (GET_WORLD) to hybrid peers, where
    // there is no per-client conversation to isolate.
    Listen,
};

struct HostConfig {
    HostMode mode = HostMode::Dedicated;
    // Where the handshake (Dedicated) or the only socket (Listen) binds.
    // "tcp://127.0.0.1:*" binds an ephemeral port; boundEndpoint() reports it.
    std::string bindEndpoint = "tcp://*:5555";
    // Dedicated only. Workers bind on 0.0.0.0, which nobody else can dial, so
    // WELCOME carries each worker's port with this host instead. Use the
    // machine's LAN address when clients run on other machines. Wildcards
    // ("*", "0.0.0.0", "::") are rejected: no client can dial them.
    std::string advertiseHost = "127.0.0.1";
    // How often server-owned platforms advance and idle players expire.
    std::chrono::milliseconds tickInterval{16};
    // Receive timeout on every socket: the longest a loop waits before it
    // notices stop().
    std::chrono::milliseconds pollInterval{200};
    // Dedicated only. How long a JOIN waits for its new worker to bind before
    // the handshake gives up on it and answers with an error, so one worker
    // that fails to start cannot hold up every other client's JOIN.
    std::chrono::milliseconds workerStartTimeout{2000};
    // Optional progress lines ("player 3 -> tcp://…"). Called from the host's
    // own threads; empty means silent.
    std::function<void(const std::string&)> log;
    // When positive, every interval the host logs each player's accepted
    // POSITION rate through `log` — the server-side evidence that clients
    // running at different speeds really do send at different rates. Zero
    // turns it off.
    std::chrono::milliseconds trafficLogInterval{0};
};

class NetworkServerHost {
public:
    // Throws std::invalid_argument if either config is unusable. Nothing is
    // bound until start().
    NetworkServerHost(ServerConfig serverConfig, HostConfig hostConfig);
    // Calls stop().
    ~NetworkServerHost();

    NetworkServerHost(const NetworkServerHost&) = delete;
    NetworkServerHost& operator=(const NetworkServerHost&) = delete;

    // Binds on the calling thread, so a taken address is reported here, as a
    // std::runtime_error, rather than on a background thread nobody is
    // watching. Then starts serving. A host starts once; std::logic_error on a
    // second call.
    void start();

    // Stops serving and joins every thread the host started. Safe to call more
    // than once, and before start().
    void stop();

    bool running() const;

    // The address actually bound, with any ephemeral port filled in. Empty
    // before start().
    std::string boundEndpoint() const;

    std::size_t playerCount() const;

    // Accepted POSITION counts per player; see NetworkServer::traffic().
    std::vector<PlayerTraffic> traffic() const;

    // Worker threads the host currently owns (Dedicated). A worker finishes
    // after an accepted LEAVE, a failed JOIN, an error, or once the server has
    // expired its player for inactivity (a client that crashed or vanished
    // without LEAVE). Finished workers are joined and dropped while the host
    // runs, so this does not grow with every client that has ever connected.
    std::size_t activeWorkers() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Network
