#pragma once

#include "NetworkDataFormat.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkSnapshotStrategy.hpp"
#include "SnapshotDelta.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Network {

// A binary message is transported as exactly one ZeroMQ frame. The codec is
// deliberately transport-free so it can be tested independently.
using BinaryMessage = std::string;

constexpr std::uint8_t binaryFormatVersion = 1;
constexpr std::size_t maxBinaryMessageSize = 16U * 1024U * 1024U;

// Delivery metadata belongs to the client/host exchange, not to the
// authoritative NetworkServer request. Existing Request/Reply codec overloads
// below remain the byte-compatible full-snapshot API.
struct BinaryRequestEnvelope {
    Request request{};
    Net::SnapshotStrategy snapshotStrategy = Net::SnapshotStrategy::FullSnapshot;
    std::uint64_t acknowledgedServerTick = 0;
    bool forceFullSnapshot = false;
};

enum class BinaryReplyKind {
    Standard,
    DeltaWelcome,
    SnapshotDelta,
};

struct BinaryReplyEnvelope {
    BinaryReplyKind kind = BinaryReplyKind::Standard;
    Reply reply{};
    SnapshotDelta delta{};
};

// Encoding validates the same semantic constraints as decoding and throws
// std::invalid_argument when a typed value cannot be represented safely.
BinaryMessage encodeBinaryRequest(const Request& request);
BinaryMessage encodeBinaryReply(const Reply& reply);

BinaryMessage encodeBinaryRequest(const BinaryRequestEnvelope& envelope);
BinaryMessage encodeBinaryReply(const BinaryReplyEnvelope& envelope);

bool decodeBinaryRequest(const BinaryMessage& message, Request& request, std::string& error);
bool decodeBinaryReply(const BinaryMessage& message, Reply& reply, std::string& error);

bool decodeBinaryRequest(const BinaryMessage& message, BinaryRequestEnvelope& envelope,
                         std::string& error);
bool decodeBinaryReply(const BinaryMessage& message, BinaryReplyEnvelope& envelope,
                       std::string& error);

// Used by the host in Phase 3 to distinguish a binary frame from multipart
// text without partially decoding either representation.
bool hasBinaryMagic(const BinaryMessage& message);

} // namespace Network
