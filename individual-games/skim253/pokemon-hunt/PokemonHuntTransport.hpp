#pragma once

#include "NetworkProtocol.hpp"

#include <chrono>
#include <memory>
#include <string>

// The only part of Pokemon Hunt's server that touches ZeroMQ.
//
// The engine's NetworkServerHost has no way to change a reply before it goes
// out, and this server has to put the boss into every snapshot, so it runs its
// own handshake and workers. What it needs from the transport is small: REP
// sockets that bind, receive and send engine Messages, and a context that
// wakes every blocked receive on shutdown.
//
// ZeroMQ stays in PokemonHuntTransport.cpp, so code that includes this header
// never sees it.
namespace PokemonHunt {

// Owns the ZeroMQ context every socket of this server is made from.
class Transport {
public:
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Wakes every blocked receive so its loop can exit. Safe to call twice.
    void shutdown() noexcept;

private:
    friend class ReplySocket;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A REP socket that wakes every `receiveTimeout` so a cancelled loop can exit.
// Like any ZeroMQ socket, use it only on the thread that created it.
class ReplySocket {
public:
    ReplySocket(Transport& transport, std::chrono::milliseconds receiveTimeout);
    ~ReplySocket();

    ReplySocket(const ReplySocket&) = delete;
    ReplySocket& operator=(const ReplySocket&) = delete;

    // Binds and returns the address actually bound, with any ephemeral port
    // filled in. Throws std::runtime_error if the address is taken.
    std::string bind(const std::string& endpoint);

    // False on a timeout, or once the transport is shutting down.
    bool receive(Network::Message& message);

    bool send(const Network::Message& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PokemonHunt
