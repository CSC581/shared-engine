#pragma once

#include "WireFormat.hpp"

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <iterator>
#include <vector>

// Carrying a list of text fields over a ZeroMQ socket, in both directions.
//
// Every socket in the engine — the server's handshake and per-client workers,
// the non-blocking client, the peer mesh, and the tests — needs exactly this
// conversion between Net::Frames and a multipart ZeroMQ message. It was
// written out five times before it was written down once; the blocking and
// non-blocking variants differ only in a flag, so they are the same function
// with an argument rather than different functions.
//
// Separate from WireFormat.hpp because this is the only place ZeroMQ appears:
// a protocol module can encode and decode messages, and be tested doing it,
// without linking a transport at all.
namespace Net {

// Sends every field as one part of a multipart message. Returns false if the
// socket refused it, which for a non-blocking send means the queue was full
// and for a closed or terminating socket means it is going away. Never throws.
inline bool send(zmq::socket_t& socket, const Frames& frames,
                 zmq::send_flags flags = zmq::send_flags::none)
{
    std::vector<zmq::const_buffer> buffers;
    buffers.reserve(frames.size());
    for (const std::string& field : frames) {
        buffers.push_back(zmq::buffer(field));
    }
    try {
        return zmq::send_multipart(socket, buffers, flags).has_value();
    } catch (const zmq::error_t&) {
        return false;
    }
}

// Receives one multipart message. `received` distinguishes "nothing was
// waiting" — a non-blocking read on an empty socket, or a blocking read that
// hit its receive timeout — from a message that genuinely had no parts. The
// two mean very different things to a caller polling in a loop, and a bare
// empty return cannot tell them apart.
//
// Propagates zmq::error_t, which is how a caller learns the context is
// shutting down; callers that poll in a loop catch it and exit.
inline Frames receive(zmq::socket_t& socket, bool& received,
                      zmq::recv_flags flags = zmq::recv_flags::none)
{
    std::vector<zmq::message_t> raw;
    received = zmq::recv_multipart(socket, std::back_inserter(raw), flags).has_value();
    if (!received) {
        return {};
    }

    Frames frames;
    frames.reserve(raw.size());
    for (const zmq::message_t& field : raw) {
        frames.push_back(field.to_string());
    }
    return frames;
}

} // namespace Net
