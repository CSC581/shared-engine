#include "BinaryNetworkProtocol.hpp"

#include "WireFormat.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

bool expect(bool condition, const std::string& message)
{
    if (!condition) std::cerr << "Test failed: " << message << '\n';
    return condition;
}

std::string hex(const std::string& bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

Network::BinaryRequestEnvelope joinEnvelope()
{
    Network::BinaryRequestEnvelope envelope;
    envelope.snapshotStrategy = Net::SnapshotStrategy::PropertyDelta;
    envelope.request.type = Network::RequestType::Join;
    envelope.request.sessionToken.assign(32, 'a');
    envelope.request.playerName = "Ada";
    return envelope;
}

Network::BinaryRequestEnvelope positionEnvelope()
{
    Network::BinaryRequestEnvelope envelope;
    envelope.snapshotStrategy = Net::SnapshotStrategy::PropertyDelta;
    envelope.acknowledgedServerTick = 0x0102030405060708ULL;
    envelope.forceFullSnapshot = true;
    envelope.request.type = Network::RequestType::Position;
    envelope.request.playerId = 0x11223344U;
    envelope.request.sessionToken.assign(32, 'b');
    envelope.request.position = {-1.25F, 2.5F, 9, std::string("x\0y", 3)};
    return envelope;
}

Network::SnapshotDelta sampleDelta()
{
    Network::SnapshotDelta delta;
    delta.baseTick = 10;
    delta.targetTick = 11;
    delta.targetPlayerCount = 2;
    delta.targetPlatformCount = 1;
    delta.addedPlayers.push_back({2, "two", 2.0F, 20.0F, "new"});
    delta.removedPlayers.push_back(3);
    Network::PlayerPatch player;
    player.id = 1;
    player.fields = static_cast<std::uint8_t>(Network::PlayerField::Name) |
                    static_cast<std::uint8_t>(Network::PlayerField::X) |
                    static_cast<std::uint8_t>(Network::PlayerField::Y) |
                    static_cast<std::uint8_t>(Network::PlayerField::Data);
    player.name = "one";
    player.x = -0.0F;
    player.y = 4.0F;
    player.data.assign(Net::maxPlayerDataLength, '\0');
    delta.changedPlayers.push_back(std::move(player));
    delta.addedPlatforms.push_back({9, 1.0F, 2.0F, 3.0F, 4.0F});
    delta.removedPlatforms.push_back(8);
    Network::PlatformPatch platform;
    platform.id = 7;
    platform.fields = 0x0FU;
    platform.x = 5.0F;
    platform.y = 6.0F;
    platform.width = 7.0F;
    platform.height = 8.0F;
    delta.changedPlatforms.push_back(platform);
    return delta;
}

bool requestRoundTripsAndGoldenBytes()
{
    bool passed = true;
    const auto join = joinEnvelope();
    const std::string encodedJoin = Network::encodeBinaryRequest(join);
    passed &= expect(
        hex(encodedJoin) ==
            "53454e47010006050020"
            "6161616161616161616161616161616161616161616161616161616161616161"
            "0003416461",
        "delta JOIN golden bytes should remain stable");

    Network::BinaryRequestEnvelope decoded;
    std::string error;
    passed &= expect(Network::decodeBinaryRequest(encodedJoin, decoded, error) &&
                         decoded.snapshotStrategy == Net::SnapshotStrategy::PropertyDelta &&
                         decoded.request.type == Network::RequestType::Join &&
                         decoded.request.sessionToken == join.request.sessionToken &&
                         decoded.request.playerName == "Ada",
                     "delta JOIN should round trip: " + error);

    const auto position = positionEnvelope();
    const std::string encodedPosition = Network::encodeBinaryRequest(position);
    passed &= expect(
        hex(encodedPosition) ==
            "53454e4701000606112233440020"
            "6262626262626262626262626262626262626262626262626262626262626262"
            "0102030405060708010000000000000009bfa00000402000000003780079",
        "delta POSITION golden bytes should remain stable");
    passed &= expect(Network::decodeBinaryRequest(encodedPosition, decoded, error) &&
                         decoded.snapshotStrategy == Net::SnapshotStrategy::PropertyDelta &&
                         decoded.acknowledgedServerTick == position.acknowledgedServerTick &&
                         decoded.forceFullSnapshot && decoded.request.playerId == 0x11223344U &&
                         decoded.request.position.data == std::string("x\0y", 3),
                     "delta POSITION should round trip: " + error);
    return passed;
}

bool replyRoundTripsAndGoldenBytes()
{
    bool passed = true;
    Network::BinaryReplyEnvelope welcome;
    welcome.kind = Network::BinaryReplyKind::DeltaWelcome;
    welcome.reply.type = Network::ReplyType::Welcome;
    welcome.reply.playerId = 1;
    welcome.reply.snapshot.serverTick = 2;
    const std::string encodedWelcome = Network::encodeBinaryReply(welcome);
    passed &= expect(hex(encodedWelcome) ==
                         "53454e470100068600000001000000000000000000020000000000000000",
                     "delta WELCOME golden bytes should remain stable");

    Network::BinaryReplyEnvelope decoded;
    std::string error;
    passed &= expect(Network::decodeBinaryReply(encodedWelcome, decoded, error) &&
                         decoded.kind == Network::BinaryReplyKind::DeltaWelcome &&
                         decoded.reply.playerId == 1 && decoded.reply.snapshot.serverTick == 2,
                     "delta WELCOME should round trip: " + error);

    Network::BinaryReplyEnvelope reply;
    reply.kind = Network::BinaryReplyKind::SnapshotDelta;
    reply.delta = sampleDelta();
    const std::string encoded = Network::encodeBinaryReply(reply);
    passed &= expect(Network::decodeBinaryReply(encoded, decoded, error) &&
                         decoded.kind == Network::BinaryReplyKind::SnapshotDelta &&
                         Network::encodeBinaryReply(decoded) == encoded,
                     "snapshot delta should round trip every section: " + error);

    Network::BinaryReplyEnvelope empty;
    empty.kind = Network::BinaryReplyKind::SnapshotDelta;
    empty.delta.baseTick = 1;
    empty.delta.targetTick = 2;
    const std::string encodedEmpty = Network::encodeBinaryReply(empty);
    passed &= expect(
        hex(encodedEmpty) ==
            "53454e4701000687000000000000000100000000000000020000000000000000"
            "000000000000000000000000000000000000000000000000",
        "empty snapshot delta golden bytes should remain stable");
    return passed;
}

bool legacyOverloadsRemainCompatible()
{
    bool passed = true;
    Network::Request request;
    request.type = Network::RequestType::Join;
    request.sessionToken.assign(32, 'a');
    request.playerName = "Ada";
    const std::string oldEncoding = Network::encodeBinaryRequest(request);
    passed &= expect(
        hex(oldEncoding) ==
            "53454e47010006010020"
            "6161616161616161616161616161616161616161616161616161616161616161"
            "0003416461",
        "legacy binary JOIN bytes must remain unchanged");
    Network::BinaryRequestEnvelope envelope;
    envelope.request = request;
    passed &= expect(Network::encodeBinaryRequest(envelope) == oldEncoding,
                     "full request envelope should preserve legacy bytes");

    Network::Request legacyDecoded;
    std::string error;
    passed &= expect(!Network::decodeBinaryRequest(Network::encodeBinaryRequest(joinEnvelope()),
                                                    legacyDecoded, error),
                     "legacy request overload should reject delta message types");

    Network::Reply reply;
    reply.type = Network::ReplyType::Snapshot;
    reply.snapshot.serverTick = 4;
    const std::string oldReply = Network::encodeBinaryReply(reply);
    passed &= expect(hex(oldReply) ==
                         "53454e470100068200000000000000040000000000000000",
                     "legacy binary SNAPSHOT bytes must remain unchanged");
    Network::BinaryReplyEnvelope replyEnvelope;
    replyEnvelope.reply = reply;
    passed &= expect(Network::encodeBinaryReply(replyEnvelope) == oldReply,
                     "standard reply envelope should preserve legacy bytes");
    return passed;
}

bool malformedPacketsAreRejected()
{
    bool passed = true;
    std::string error;
    Network::BinaryRequestEnvelope request;
    std::string position = Network::encodeBinaryRequest(positionEnvelope());
    position[54] = static_cast<char>(0x80);
    passed &= expect(!Network::decodeBinaryRequest(position, request, error),
                     "unknown delta POSITION flags should be rejected");

    position = Network::encodeBinaryRequest(positionEnvelope());
    for (std::size_t i = 46; i < 54; ++i) position[i] = 0;
    passed &= expect(!Network::decodeBinaryRequest(position, request, error),
                     "zero acknowledged tick should be rejected");

    const std::string validJoin = Network::encodeBinaryRequest(joinEnvelope());
    for (std::size_t size = 0; size < validJoin.size(); ++size) {
        passed &= expect(!Network::decodeBinaryRequest(validJoin.substr(0, size), request, error),
                         "every truncated delta JOIN should be rejected");
    }

    Network::BinaryReplyEnvelope reply;
    Network::BinaryReplyEnvelope valid;
    valid.kind = Network::BinaryReplyKind::SnapshotDelta;
    valid.delta.baseTick = 1;
    valid.delta.targetTick = 2;
    std::string packet = Network::encodeBinaryReply(valid);
    packet.append("x");
    passed &= expect(!Network::decodeBinaryReply(packet, reply, error),
                     "trailing delta bytes should be rejected");

    packet = Network::encodeBinaryReply(valid);
    packet[32] = static_cast<char>(0x7F);
    packet[33] = static_cast<char>(0xFF);
    packet[34] = static_cast<char>(0xFF);
    packet[35] = static_cast<char>(0xFF);
    passed &= expect(!Network::decodeBinaryReply(packet, reply, error),
                     "impossible delta counts should be rejected before allocation");

    Network::BinaryReplyEnvelope changed;
    changed.kind = Network::BinaryReplyKind::SnapshotDelta;
    changed.delta.baseTick = 1;
    changed.delta.targetTick = 2;
    changed.delta.targetPlayerCount = 1;
    Network::PlayerPatch patch;
    patch.id = 1;
    patch.fields = static_cast<std::uint8_t>(Network::PlayerField::X);
    patch.x = 1.0F;
    changed.delta.changedPlayers.push_back(patch);
    packet = Network::encodeBinaryReply(changed);
    packet[48] = static_cast<char>(0x80);
    passed &= expect(!Network::decodeBinaryReply(packet, reply, error),
                     "unknown player patch masks should be rejected");

    packet = Network::encodeBinaryReply(changed);
    packet[49] = static_cast<char>(0x7F);
    packet[50] = static_cast<char>(0x80);
    packet[51] = 0;
    packet[52] = 0;
    passed &= expect(!Network::decodeBinaryReply(packet, reply, error),
                     "non-finite changed properties should be rejected");

    changed.delta.changedPlayers[0].x = 1.0F;
    changed.delta.removedPlayers.push_back(2);
    changed.delta.changedPlayers[0].id = 1;
    packet = Network::encodeBinaryReply(changed);
    // The packet is valid with removed ID 2 and changed ID 1. Rewriting the
    // changed ID to 2 makes one ID appear in both sections.
    packet[48] = 0;
    packet[49] = 0;
    packet[50] = 0;
    packet[51] = 2;
    passed &= expect(!Network::decodeBinaryReply(packet, reply, error),
                     "IDs repeated across delta sections should be rejected");

    packet = Network::encodeBinaryReply(valid);
    packet[6] = 7;
    passed &= expect(!Network::decodeBinaryReply(packet, reply, error) &&
                         error == "unsupported protocol version",
                     "incompatible delta protocol versions should be rejected explicitly");

    std::string oversized(Network::maxBinaryMessageSize + 1U, '\0');
    passed &= expect(!Network::decodeBinaryReply(oversized, reply, error),
                     "oversized delta packets should be rejected");
    return passed;
}

bool invalidTypedValuesAreRejected()
{
    bool passed = true;
    const auto throws = [&](Network::BinaryReplyEnvelope reply, const std::string& description) {
        try {
            (void)Network::encodeBinaryReply(reply);
            return expect(false, description + " should throw while encoding");
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    Network::BinaryReplyEnvelope reply;
    reply.kind = Network::BinaryReplyKind::SnapshotDelta;
    reply.delta = sampleDelta();
    reply.delta.removedPlayers.push_back(reply.delta.changedPlayers[0].id);
    passed &= throws(reply, "ID repeated across delta sections");

    reply.delta = sampleDelta();
    reply.delta.changedPlatforms[0].width = std::numeric_limits<float>::infinity();
    passed &= throws(reply, "non-finite selected platform property");

    try {
        auto request = positionEnvelope();
        request.acknowledgedServerTick = 0;
        (void)Network::encodeBinaryRequest(request);
        passed &= expect(false, "zero acknowledged tick should throw while encoding");
    } catch (const std::invalid_argument&) {
    }
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= requestRoundTripsAndGoldenBytes();
    passed &= replyRoundTripsAndGoldenBytes();
    passed &= legacyOverloadsRemainCompatible();
    passed &= malformedPacketsAreRejected();
    passed &= invalidTypedValuesAreRejected();
    if (passed) std::cout << "Snapshot delta codec tests passed\n";
    return passed ? 0 : 1;
}
