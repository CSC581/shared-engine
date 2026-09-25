#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

// Endpoint string handling shared by every networking module in the engine.
//
// Header-only and dependency-free on purpose: the client-server module
// (Network) and the peer-to-peer module (Peer) both need it, and neither
// should have to link the other to get one small string utility.
namespace Net {

// Replace the host in a tcp://host:port endpoint while keeping the port.
// A socket bound to "tcp://0.0.0.0:*" reports a bound endpoint that is correct
// locally and useless to anyone else; this turns it into the address a remote
// process can actually dial. Returns an empty string if endpoint/host is not a
// usable tcp://…:port pair.
inline std::string rewriteTcpEndpointHost(const std::string& endpoint, const std::string& host)
{
    constexpr const char* kPrefix = "tcp://";
    constexpr std::size_t kPrefixLen = 6;
    if (host.empty() || endpoint.size() <= kPrefixLen ||
        endpoint.compare(0, kPrefixLen, kPrefix) != 0) {
        return {};
    }

    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon <= kPrefixLen) {
        return {};
    }

    const std::string port = endpoint.substr(colon + 1);
    if (port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        })) {
        return {};
    }

    // Bracket IPv6 advertise hosts so "tcp://::1:5555" stays unambiguous.
    if (host.find(':') != std::string::npos && !(host.front() == '[' && host.back() == ']')) {
        return std::string(kPrefix) + '[' + host + "]:" + port;
    }
    return std::string(kPrefix) + host + ':' + port;
}

} // namespace Net
