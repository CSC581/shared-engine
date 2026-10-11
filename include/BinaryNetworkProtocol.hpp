#pragma once

#include "NetworkDataFormat.hpp"
#include "NetworkProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Network {

// A binary message is transported as exactly one ZeroMQ frame. The codec is
// deliberately transport-free so it can be tested independently.
using BinaryMessage = std::string;

constexpr std::uint8_t binaryFormatVersion = 1;
constexpr std::size_t maxBinaryMessageSize = 16U * 1024U * 1024U;

// Encoding validates the same semantic constraints as decoding and throws
// std::invalid_argument when a typed value cannot be represented safely.
BinaryMessage encodeBinaryRequest(const Request& request);
BinaryMessage encodeBinaryReply(const Reply& reply);

bool decodeBinaryRequest(const BinaryMessage& message, Request& request, std::string& error);
bool decodeBinaryReply(const BinaryMessage& message, Reply& reply, std::string& error);

// Used by the host in Phase 3 to distinguish a binary frame from multipart
// text without partially decoding either representation.
bool hasBinaryMagic(const BinaryMessage& message);

} // namespace Network
