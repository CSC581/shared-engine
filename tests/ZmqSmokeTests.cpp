// Build-level smoke test for the vendored ZeroMQ import (libzmq + cppzmq).
//
// This does not test engine logic. It exists so that a broken vendor wiring --
// missing submodule, wrong link target, unusable headers -- fails here on every
// machine instead of surfacing later inside networking code.

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <array>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }

    return condition;
}

} // namespace

int main()
{
    bool passed = true;

    // --- the C library we actually linked against ---------------------------
    int major = 0;
    int minor = 0;
    int patch = 0;
    zmq_version(&major, &minor, &patch);

    passed &= expect(major >= 4, "linked libzmq should be version 4 or newer");

    // --- REQ/REP round trip over a real TCP socket --------------------------
    // Port 0 ("*") lets the OS pick a free port, so parallel or repeated runs
    // never collide on a hard-coded one.
    zmq::context_t context(1);

    zmq::socket_t server(context, zmq::socket_type::rep);
    server.bind("tcp://127.0.0.1:*");
    const std::string endpoint = server.get(zmq::sockopt::last_endpoint);

    passed &= expect(!endpoint.empty(), "bound socket should report an endpoint");

    zmq::socket_t client(context, zmq::socket_type::req);
    client.connect(endpoint);

    // Never let a wiring failure hang the suite; fail the test instead.
    server.set(zmq::sockopt::rcvtimeo, 5000);
    client.set(zmq::sockopt::rcvtimeo, 5000);

    client.send(zmq::str_buffer("ping"), zmq::send_flags::none);

    zmq::message_t request;
    const auto received = server.recv(request, zmq::recv_flags::none);

    passed &= expect(received.has_value(), "server should receive the request");
    passed &= expect(request.to_string() == "ping",
                     "server should receive the bytes the client sent");

    server.send(zmq::str_buffer("pong"), zmq::send_flags::none);

    zmq::message_t reply;
    const auto replied = client.recv(reply, zmq::recv_flags::none);

    passed &= expect(replied.has_value(), "client should receive the reply");
    passed &= expect(reply.to_string() == "pong",
                     "client should receive the bytes the server sent");

    // --- multipart, which lives in zmq_addon.hpp ----------------------------
    // Exercised separately so a missing/!broken addon header fails loudly; the
    // networking code will lean on multipart for state updates.
    std::array<zmq::const_buffer, 2> parts{zmq::str_buffer("pos"),
                                           zmq::str_buffer("12,34")};
    passed &= expect(zmq::send_multipart(client, parts).has_value(),
                     "multipart send should report success");

    std::vector<zmq::message_t> incoming;
    const auto count = zmq::recv_multipart(server, std::back_inserter(incoming));

    passed &= expect(count.has_value() && *count == 2,
                     "server should receive both message parts");

    if (passed) {
        std::cout << "zmq smoke: libzmq " << major << '.' << minor << '.' << patch
                  << " via cppzmq, round trip ok\n";
    }

    return passed ? 0 : 1;
}
