#include "BinaryNetworkProtocol.hpp"
#include "NetworkProtocol.hpp"
#include "WireFormat.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

bool equal(const Network::PlatformState& left, const Network::PlatformState& right)
{
    return left.id == right.id && left.x == right.x && left.y == right.y &&
           left.width == right.width && left.height == right.height;
}

bool equal(const Network::PlayerState& left, const Network::PlayerState& right)
{
    return left.id == right.id && left.name == right.name && left.x == right.x &&
           left.y == right.y && left.data == right.data;
}

bool equal(const Network::WorldSnapshot& left, const Network::WorldSnapshot& right)
{
    if (left.serverTick != right.serverTick || left.players.size() != right.players.size() ||
        left.platforms.size() != right.platforms.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.players.size(); ++index) {
        if (!equal(left.players[index], right.players[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.platforms.size(); ++index) {
        if (!equal(left.platforms[index], right.platforms[index])) {
            return false;
        }
    }
    return true;
}

bool equal(const Network::Request& left, const Network::Request& right)
{
    return left.type == right.type && left.playerId == right.playerId &&
           left.sessionToken == right.sessionToken && left.playerName == right.playerName &&
           left.position.x == right.position.x && left.position.y == right.position.y &&
           left.position.sequence == right.position.sequence && left.position.data == right.position.data;
}

bool equal(const Network::Reply& left, const Network::Reply& right)
{
    return left.type == right.type && left.playerId == right.playerId &&
           left.sessionEndpoint == right.sessionEndpoint && equal(left.snapshot, right.snapshot) &&
           left.worldState.worldRevision == right.worldState.worldRevision &&
           left.worldState.platforms.size() == right.worldState.platforms.size() &&
           left.error == right.error &&
           [&]() {
               for (std::size_t index = 0; index < left.worldState.platforms.size(); ++index) {
                   if (!equal(left.worldState.platforms[index], right.worldState.platforms[index])) {
                       return false;
                   }
               }
               return true;
           }();
}

Network::Message encodeTextRequest(const Network::Request& request)
{
    switch (request.type) {
    case Network::RequestType::Join:
        return Network::encodeJoin(request.sessionToken, request.playerName);
    case Network::RequestType::Position:
        return Network::encodePosition(request.playerId, request.sessionToken, request.position);
    case Network::RequestType::Leave:
        return Network::encodeLeave(request.playerId, request.sessionToken);
    case Network::RequestType::GetWorld:
        return Network::encodeGetWorld();
    }
    return {};
}

Network::WorldSnapshot sampleSnapshot()
{
    return {0x0102030405060708ULL,
            {{1, "Ada", -12.5F, 34.25F, std::string("opaque\0bytes", 12)},
             {0xA0B0C0D0U, "", 0.125F, -4096.5F, {}}},
            {{7, 10.0F, 20.0F, 80.0F, 16.0F},
             {9, -1.25F, 2.5F, 3.75F, 4.125F}}};
}

std::vector<Network::Request> sampleRequests()
{
    const Network::SessionToken token(32, 'a');
    Network::Request join;
    join.type = Network::RequestType::Join;
    join.sessionToken = token;
    join.playerName = "Ada";

    Network::Request position;
    position.type = Network::RequestType::Position;
    position.playerId = 0x01020304U;
    position.sessionToken = token;
    position.position = {-1.25F, 9876.5F, 0x0102030405060708ULL,
                         std::string("raw\0data", 8)};

    Network::Request leave;
    leave.type = Network::RequestType::Leave;
    leave.playerId = 0x89ABCDEFU;
    leave.sessionToken = token;

    Network::Request world;
    world.type = Network::RequestType::GetWorld;
    return {join, position, leave, world};
}

std::vector<Network::Reply> sampleReplies()
{
    const Network::WorldSnapshot snapshot = sampleSnapshot();

    Network::Reply welcome;
    welcome.type = Network::ReplyType::Welcome;
    welcome.playerId = 42;
    welcome.sessionEndpoint = "tcp://127.0.0.1:5555";
    welcome.snapshot = snapshot;

    Network::Reply snapshotReply;
    snapshotReply.type = Network::ReplyType::Snapshot;
    snapshotReply.snapshot = snapshot;

    Network::Reply world;
    world.type = Network::ReplyType::WorldState;
    world.worldState = {12345, snapshot.platforms};

    Network::Reply error;
    error.type = Network::ReplyType::Error;
    error.error = std::string("bad\0request", 11);

    Network::Reply goodbye;
    goodbye.type = Network::ReplyType::Goodbye;
    return {welcome, snapshotReply, world, error, goodbye};
}

bool roundTripsEveryMessage()
{
    bool passed = true;
    for (const Network::Request& expected : sampleRequests()) {
        const Network::BinaryMessage encoded = Network::encodeBinaryRequest(expected);
        Network::Request actual;
        std::string error;
        passed &= expect(Network::decodeBinaryRequest(encoded, actual, error),
                         "binary request should round-trip: " + error);
        passed &= expect(equal(actual, expected), "binary request should preserve every field");
    }

    for (const Network::Reply& expected : sampleReplies()) {
        const Network::BinaryMessage encoded = Network::encodeBinaryReply(expected);
        Network::Reply actual;
        std::string error;
        passed &= expect(Network::decodeBinaryReply(encoded, actual, error),
                         "binary reply should round-trip: " + error);
        passed &= expect(equal(actual, expected), "binary reply should preserve every field");
    }
    return passed;
}

bool matchesTextSemantics()
{
    bool passed = true;
    for (const Network::Request& expected : sampleRequests()) {
        Network::Request text;
        Network::Request binary;
        std::string error;
        passed &= expect(Network::decodeRequest(encodeTextRequest(expected), text, error),
                         "text request should decode for equivalence test");
        passed &= expect(Network::decodeBinaryRequest(Network::encodeBinaryRequest(expected), binary, error),
                         "binary request should decode for equivalence test");
        passed &= expect(equal(text, binary), "text and binary requests should have identical semantics");
    }

    for (const Network::Reply& expected : sampleReplies()) {
        Network::Reply text;
        Network::Reply binary;
        std::string error;
        passed &= expect(Network::decodeReply(Network::encodeReply(expected), text, error),
                         "text reply should decode for equivalence test");
        passed &= expect(Network::decodeBinaryReply(Network::encodeBinaryReply(expected), binary, error),
                         "binary reply should decode for equivalence test");
        passed &= expect(equal(text, binary), "text and binary replies should have identical semantics");
    }
    return passed;
}

bool usesStableNetworkByteOrder()
{
    Network::Request world;
    world.type = Network::RequestType::GetWorld;
    const Network::BinaryMessage header = Network::encodeBinaryRequest(world);
    bool passed = expect(header == std::string("SENG\x01\x00\x06\x04", 8),
                         "binary header should contain magic, codec version, semantic version and type");
    passed &= expect(Network::hasBinaryMagic(header) && !Network::hasBinaryMagic("SEN"),
                     "binary magic detection should require the complete marker");

    const Network::Request position = sampleRequests()[1];
    const Network::BinaryMessage bytes = Network::encodeBinaryRequest(position);
    passed &= expect(static_cast<unsigned char>(bytes[8]) == 0x01 &&
                         static_cast<unsigned char>(bytes[9]) == 0x02 &&
                         static_cast<unsigned char>(bytes[10]) == 0x03 &&
                         static_cast<unsigned char>(bytes[11]) == 0x04,
                     "fixed-width integers should use big-endian byte order");
    passed &= expect(static_cast<unsigned char>(bytes[54]) == 0xBF &&
                         static_cast<unsigned char>(bytes[55]) == 0xA0 &&
                         static_cast<unsigned char>(bytes[56]) == 0x00 &&
                         static_cast<unsigned char>(bytes[57]) == 0x00,
                     "floats should use their IEEE-754 binary32 bits in big-endian order");
    return passed;
}

bool acceptsBoundaryLengths()
{
    Network::Request join;
    join.type = Network::RequestType::Join;
    join.sessionToken = std::string(32, 'F');
    join.playerName = std::string(Net::maxNameLength, 'n');

    Network::Request position;
    position.type = Network::RequestType::Position;
    position.playerId = std::numeric_limits<Network::PlayerId>::max();
    position.sessionToken = std::string(32, '0');
    position.position = {std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<std::uint64_t>::max(),
                         std::string(Net::maxPlayerDataLength, '\0')};

    bool passed = true;
    for (const Network::Request* expected : {&join, &position}) {
        Network::Request actual;
        std::string error;
        passed &= expect(Network::decodeBinaryRequest(Network::encodeBinaryRequest(*expected), actual, error),
                         "maximum legal request fields should round-trip");
        passed &= expect(equal(*expected, actual), "maximum legal request fields should be preserved");
    }
    return passed;
}

bool rejectsTruncationAndWrongDirection()
{
    bool passed = true;
    for (const Network::Request& request : sampleRequests()) {
        const Network::BinaryMessage bytes = Network::encodeBinaryRequest(request);
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            Network::Request decoded;
            std::string error;
            passed &= expect(!Network::decodeBinaryRequest(bytes.substr(0, size), decoded, error),
                             "every truncated request prefix should be rejected");
        }
    }
    for (const Network::Reply& reply : sampleReplies()) {
        const Network::BinaryMessage bytes = Network::encodeBinaryReply(reply);
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            Network::Reply decoded;
            std::string error;
            passed &= expect(!Network::decodeBinaryReply(bytes.substr(0, size), decoded, error),
                             "every truncated reply prefix should be rejected");
        }
    }

    std::string error;
    Network::Request request;
    Network::Reply reply;
    passed &= expect(!Network::decodeBinaryRequest(Network::encodeBinaryReply(sampleReplies()[4]), request, error),
                     "a reply should not decode as a request");
    passed &= expect(!Network::decodeBinaryReply(Network::encodeBinaryRequest(sampleRequests()[3]), reply, error),
                     "a request should not decode as a reply");
    return passed;
}

bool rejectsMalformedHeadersAndBodies()
{
    bool passed = true;
    Network::Request decodedRequest;
    Network::Reply decodedReply;
    std::string error;

    Network::BinaryMessage badMagic = Network::encodeBinaryRequest(sampleRequests()[3]);
    badMagic[0] = 'X';
    passed &= expect(!Network::decodeBinaryRequest(badMagic, decodedRequest, error), "wrong magic should be rejected");

    Network::BinaryMessage badFormat = Network::encodeBinaryRequest(sampleRequests()[3]);
    badFormat[4] = static_cast<char>(Network::binaryFormatVersion + 1);
    passed &= expect(!Network::decodeBinaryRequest(badFormat, decodedRequest, error),
                     "incompatible binary format version should be rejected");

    Network::BinaryMessage badProtocol = Network::encodeBinaryRequest(sampleRequests()[3]);
    badProtocol[6] = static_cast<char>(Network::protocolVersion + 1);
    passed &= expect(!Network::decodeBinaryRequest(badProtocol, decodedRequest, error),
                     "incompatible semantic protocol version should be rejected");

    Network::BinaryMessage unknownType = Network::encodeBinaryRequest(sampleRequests()[3]);
    unknownType[7] = static_cast<char>(0x7F);
    passed &= expect(!Network::decodeBinaryRequest(unknownType, decodedRequest, error),
                     "unknown request type should be rejected");

    Network::BinaryMessage trailing = Network::encodeBinaryReply(sampleReplies()[4]);
    trailing.push_back('\0');
    passed &= expect(!Network::decodeBinaryReply(trailing, decodedReply, error),
                     "trailing bytes should be rejected");

    Network::BinaryMessage badPlayerCount = Network::encodeBinaryReply(sampleReplies()[1]);
    badPlayerCount[16] = static_cast<char>(0xFF);
    badPlayerCount[17] = static_cast<char>(0xFF);
    badPlayerCount[18] = static_cast<char>(0xFF);
    badPlayerCount[19] = static_cast<char>(0xFF);
    passed &= expect(!Network::decodeBinaryReply(badPlayerCount, decodedReply, error),
                     "impossible snapshot counts should be rejected before allocation");

    Network::BinaryMessage oversized(Network::maxBinaryMessageSize + 1, '\0');
    oversized.replace(0, 4, "SENG");
    passed &= expect(!Network::decodeBinaryRequest(oversized, decodedRequest, error),
                     "oversized packets should be rejected");
    return passed;
}

bool rejectsNonFiniteAndOversizedFields()
{
    bool passed = true;
    std::string error;
    Network::Request decoded;

    // POSITION x begins after header, player id, token, and sequence.
    Network::BinaryMessage infinite = Network::encodeBinaryRequest(sampleRequests()[1]);
    infinite[54] = static_cast<char>(0x7F);
    infinite[55] = static_cast<char>(0x80);
    infinite[56] = 0;
    infinite[57] = 0;
    passed &= expect(!Network::decodeBinaryRequest(infinite, decoded, error),
                     "non-finite binary floats should be rejected");

    Network::Request maximumData = sampleRequests()[1];
    maximumData.position.data.assign(Net::maxPlayerDataLength, 'd');
    Network::BinaryMessage tooMuchData = Network::encodeBinaryRequest(maximumData);
    // POSITION data length follows the two floats at byte 62.
    tooMuchData[62] = 0x02;
    tooMuchData[63] = 0x01;
    tooMuchData.push_back('d');
    passed &= expect(!Network::decodeBinaryRequest(tooMuchData, decoded, error),
                     "player data beyond the shared limit should be rejected");

    Network::Request maximumName = sampleRequests()[0];
    maximumName.playerName.assign(Net::maxNameLength, 'n');
    Network::BinaryMessage tooLongName = Network::encodeBinaryRequest(maximumName);
    // JOIN name length follows the 32-byte token at byte 42.
    tooLongName[42] = 0;
    tooLongName[43] = static_cast<char>(Net::maxNameLength + 1);
    tooLongName.push_back('n');
    passed &= expect(!Network::decodeBinaryRequest(tooLongName, decoded, error),
                     "names beyond the shared limit should be rejected");

    bool threw = false;
    Network::Request invalid = sampleRequests()[1];
    invalid.position.y = std::numeric_limits<float>::infinity();
    try {
        static_cast<void>(Network::encodeBinaryRequest(invalid));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    passed &= expect(threw, "the encoder should reject non-finite typed values");

    threw = false;
    Network::Reply oversizedError;
    oversizedError.type = Network::ReplyType::Error;
    oversizedError.error.assign(static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U, 'e');
    try {
        static_cast<void>(Network::encodeBinaryReply(oversizedError));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    passed &= expect(threw, "the encoder should reject strings that do not fit their length prefix");
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= roundTripsEveryMessage();
    passed &= matchesTextSemantics();
    passed &= usesStableNetworkByteOrder();
    passed &= acceptsBoundaryLengths();
    passed &= rejectsTruncationAndWrongDirection();
    passed &= rejectsMalformedHeadersAndBodies();
    passed &= rejectsNonFiniteAndOversizedFields();
    if (!passed) {
        return 1;
    }
    std::cout << "Binary network protocol tests passed\n";
    return 0;
}
