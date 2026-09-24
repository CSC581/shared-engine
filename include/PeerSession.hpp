#pragma once

#include "PeerProtocol.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Peer {

// One peer's half of a peer-to-peer mesh: no relay, no authority, no special
// process. Every peer runs this same class, and a message a peer publishes
// goes straight to every other peer's socket.
//
// Two sockets do the work, because a mesh needs two very different things:
//
//   PUB  — everything this peer has to say about itself (state, input,
//          checksum, leave), fanned out to everyone at once. One send reaches
//          every peer, and no peer has to acknowledge anything, which is what
//          keeps a slow peer from holding up the sender.
//   REP  — the "knock on the door" socket. PUB/SUB is one-directional, so a
//          joining peer connecting to an existing peer's PUB would hear that
//          peer without that peer ever hearing back. HELLO fixes that: the
//          newcomer introduces itself on this socket and gets the answerer's
//          whole roster in reply, while the answerer subscribes back to the
//          newcomer. One known address is therefore enough to reach a full
//          mesh, however many peers are already in it.
//
// This is deliberately not Router/Dealer. The handout bans those for the
// asynchronous server in Section 4, and building the mesh out of the plainest
// possible sockets keeps the ownership story visible: every peer publishes
// only its own data and nobody is in a position to publish anyone else's.
//
// Threading: a background thread owns each socket, because ZeroMQ sockets may
// not be touched from two threads at once. The game thread only ever calls the
// public methods here, which hand work to those threads through locked queues.
class PeerSession {
public:
    struct Config {
        PeerId id = 1;
        std::string name = "peer";
        // Bind specifications. "*" for the port lets the OS choose, and the
        // address actually bound is reported by self() after start().
        std::string pubBind = "tcp://0.0.0.0:*";
        std::string greetBind = "tcp://0.0.0.0:*";
        // Host other peers should dial. Binding on 0.0.0.0 is right for
        // listening and useless as an address to hand out, so the endpoints in
        // HELLO and ROSTER are rewritten to this host.
        std::string advertiseHost = "127.0.0.1";
        // Greet endpoints of peers already running. One is enough; the roster
        // that comes back introduces the rest.
        std::vector<std::string> bootstrap;
    };

    explicit PeerSession(Config config);

    // Publishes LEAVE, stops the threads and closes the sockets.
    ~PeerSession();

    PeerSession(const PeerSession&) = delete;
    PeerSession& operator=(const PeerSession&) = delete;

    // Binds both sockets and starts the background threads. Throws
    // std::runtime_error if the sockets cannot be bound, which is what happens
    // when two peers are told to use the same port.
    void start();

    // Broadcasts to every peer that has subscribed to us. Safe to call every
    // frame; this never blocks waiting for a peer to catch up.
    void publishState(const PeerState& state);

    // Everything received since the last call, in arrival order. The game
    // thread drains this once a frame and decides what each message means;
    // the session itself understands only membership.
    std::vector<Envelope> drain();

    // Peers currently known, including this one, ordered by id. Ordering is
    // not cosmetic: the deterministic mode needs every peer to agree on who is
    // playing, and sorting by id gives that without anyone deciding it.
    std::vector<PeerAddress> roster() const;
    bool knows(PeerId id) const;
    std::size_t peerCount() const;

    // This peer's own address, with the endpoints it actually bound. Only
    // meaningful after start().
    PeerAddress self() const;

    // Most recent transport problem, for display. Empty when healthy.
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Peer
