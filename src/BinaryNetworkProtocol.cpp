#include "BinaryNetworkProtocol.hpp"

#include "WireFormat.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Network {
namespace {

constexpr char magic[] = {'S', 'E', 'N', 'G'};
constexpr std::size_t headerSize = 8;

enum class MessageType : std::uint8_t {
    Join = 1,
    Position = 2,
    Leave = 3,
    GetWorld = 4,
    Welcome = 0x81,
    Snapshot = 0x82,
    WorldState = 0x83,
    Error = 0x84,
    Goodbye = 0x85,
};

class Writer {
public:
    explicit Writer(MessageType type)
    {
        bytes_.append(magic, sizeof(magic));
        u8(binaryFormatVersion);
        u16(static_cast<std::uint16_t>(protocolVersion));
        u8(static_cast<std::uint8_t>(type));
    }

    void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }

    void u16(std::uint16_t value)
    {
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value));
    }

    void u32(std::uint32_t value)
    {
        u8(static_cast<std::uint8_t>(value >> 24U));
        u8(static_cast<std::uint8_t>(value >> 16U));
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value));
    }

    void u64(std::uint64_t value)
    {
        u32(static_cast<std::uint32_t>(value >> 32U));
        u32(static_cast<std::uint32_t>(value));
    }

    void f32(float value)
    {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("binary protocol cannot encode a non-finite float");
        }
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "32-bit floats are required");
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void string(const std::string& value)
    {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("binary protocol string is too large");
        }
        u16(static_cast<std::uint16_t>(value.size()));
        bytes_.append(value);
    }

    BinaryMessage finish()
    {
        if (bytes_.size() > maxBinaryMessageSize) {
            throw std::invalid_argument("binary protocol message is too large");
        }
        return std::move(bytes_);
    }

private:
    BinaryMessage bytes_;
};

class Reader {
public:
    explicit Reader(const BinaryMessage& bytes)
        : bytes_(bytes)
    {
    }

    bool header(MessageType& type, std::string& error)
    {
        if (bytes_.size() > maxBinaryMessageSize) {
            error = "binary message is too large";
            return false;
        }
        if (bytes_.size() < headerSize || !hasBinaryMagic(bytes_)) {
            error = "invalid binary message header";
            return false;
        }

        index_ = sizeof(magic);
        std::uint8_t formatVersion = 0;
        std::uint16_t semanticVersion = 0;
        std::uint8_t rawType = 0;
        if (!u8(formatVersion) || !u16(semanticVersion) || !u8(rawType)) {
            error = "truncated binary message header";
            return false;
        }
        if (formatVersion != binaryFormatVersion) {
            error = "unsupported binary format version";
            return false;
        }
        if (semanticVersion != protocolVersion) {
            error = "unsupported protocol version";
            return false;
        }
        type = static_cast<MessageType>(rawType);
        return true;
    }

    bool u8(std::uint8_t& value)
    {
        if (remaining() < 1) {
            return false;
        }
        value = static_cast<std::uint8_t>(static_cast<unsigned char>(bytes_[index_++]));
        return true;
    }

    bool u16(std::uint16_t& value)
    {
        std::uint8_t high = 0;
        std::uint8_t low = 0;
        if (!u8(high) || !u8(low)) {
            return false;
        }
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) | low);
        return true;
    }

    bool u32(std::uint32_t& value)
    {
        std::uint8_t bytes[4]{};
        if (!u8(bytes[0]) || !u8(bytes[1]) || !u8(bytes[2]) || !u8(bytes[3])) {
            return false;
        }
        value = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                static_cast<std::uint32_t>(bytes[3]);
        return true;
    }

    bool u64(std::uint64_t& value)
    {
        std::uint32_t high = 0;
        std::uint32_t low = 0;
        if (!u32(high) || !u32(low)) {
            return false;
        }
        value = (static_cast<std::uint64_t>(high) << 32U) | low;
        return true;
    }

    bool f32(float& value)
    {
        std::uint32_t bits = 0;
        if (!u32(bits)) {
            return false;
        }
        static_assert(sizeof(bits) == sizeof(value), "32-bit floats are required");
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value);
    }

    bool string(std::string& value, std::size_t maximum)
    {
        std::uint16_t size = 0;
        if (!u16(size) || size > maximum || remaining() < size) {
            return false;
        }
        value.assign(bytes_.data() + index_, size);
        index_ += size;
        return true;
    }

    std::size_t remaining() const { return bytes_.size() - index_; }
    bool finished() const { return index_ == bytes_.size(); }

private:
    const BinaryMessage& bytes_;
    std::size_t index_ = 0;
};

bool validSessionToken(const std::string& token)
{
    if (token.size() != 32) {
        return false;
    }
    for (unsigned char character : token) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

void validatePlayer(const PlayerState& player)
{
    if (player.id == 0 || !Net::isValidName(player.name) || !Net::isValidPlayerData(player.data) ||
        !std::isfinite(player.x) || !std::isfinite(player.y)) {
        throw std::invalid_argument("invalid player in binary snapshot");
    }
}

void validatePlatform(const PlatformState& platform)
{
    if (platform.id == 0 || !std::isfinite(platform.x) || !std::isfinite(platform.y) ||
        !std::isfinite(platform.width) || !std::isfinite(platform.height) ||
        platform.width <= 0.0F || platform.height <= 0.0F) {
        throw std::invalid_argument("invalid platform in binary snapshot");
    }
}

void writePlatforms(Writer& writer, const std::vector<PlatformState>& platforms)
{
    if (platforms.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("too many platforms in binary snapshot");
    }
    writer.u32(static_cast<std::uint32_t>(platforms.size()));
    for (const PlatformState& platform : platforms) {
        validatePlatform(platform);
        writer.u32(platform.id);
        writer.f32(platform.x);
        writer.f32(platform.y);
        writer.f32(platform.width);
        writer.f32(platform.height);
    }
}

void writeSnapshot(Writer& writer, const WorldSnapshot& snapshot)
{
    if (snapshot.players.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("too many players in binary snapshot");
    }
    writer.u64(snapshot.serverTick);
    writer.u32(static_cast<std::uint32_t>(snapshot.players.size()));
    for (const PlayerState& player : snapshot.players) {
        validatePlayer(player);
        writer.u32(player.id);
        writer.string(player.name);
        writer.f32(player.x);
        writer.f32(player.y);
        writer.string(player.data);
    }
    writePlatforms(writer, snapshot.platforms);
}

bool readPlatforms(Reader& reader, std::vector<PlatformState>& platforms, std::string& error)
{
    constexpr std::size_t platformBytes = 5U * sizeof(std::uint32_t);
    std::uint32_t count = 0;
    if (!reader.u32(count) || count > reader.remaining() / platformBytes) {
        error = "invalid binary platform count";
        return false;
    }

    platforms.clear();
    platforms.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        PlatformState platform;
        if (!reader.u32(platform.id) || !reader.f32(platform.x) || !reader.f32(platform.y) ||
            !reader.f32(platform.width) || !reader.f32(platform.height) || platform.id == 0 ||
            platform.width <= 0.0F || platform.height <= 0.0F) {
            error = "invalid platform in binary snapshot";
            return false;
        }
        platforms.push_back(platform);
    }
    return true;
}

bool readSnapshot(Reader& reader, WorldSnapshot& snapshot, std::string& error)
{
    std::uint32_t count = 0;
    if (!reader.u64(snapshot.serverTick) || !reader.u32(count)) {
        error = "invalid binary snapshot header";
        return false;
    }

    // Each player needs at least id, two empty string lengths, and two floats.
    constexpr std::size_t minimumPlayerBytes = 4U + 2U + 4U + 4U + 2U;
    if (count > reader.remaining() / minimumPlayerBytes) {
        error = "invalid binary player count";
        return false;
    }

    snapshot.players.clear();
    snapshot.players.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        PlayerState player;
        if (!reader.u32(player.id) || !reader.string(player.name, Net::maxNameLength) ||
            !Net::isValidName(player.name) || !reader.f32(player.x) || !reader.f32(player.y) ||
            !reader.string(player.data, Net::maxPlayerDataLength) || player.id == 0) {
            error = "invalid player in binary snapshot";
            return false;
        }
        snapshot.players.push_back(std::move(player));
    }
    return readPlatforms(reader, snapshot.platforms, error);
}

bool finish(Reader& reader, std::string& error)
{
    if (!reader.finished()) {
        error = "binary message has trailing bytes";
        return false;
    }
    return true;
}

} // namespace

bool hasBinaryMagic(const BinaryMessage& message)
{
    return message.size() >= sizeof(magic) &&
           std::memcmp(message.data(), magic, sizeof(magic)) == 0;
}

BinaryMessage encodeBinaryRequest(const Request& request)
{
    switch (request.type) {
    case RequestType::Join: {
        if (!validSessionToken(request.sessionToken) || !Net::isValidName(request.playerName)) {
            throw std::invalid_argument("invalid binary JOIN request");
        }
        Writer writer(MessageType::Join);
        writer.string(request.sessionToken);
        writer.string(request.playerName);
        return writer.finish();
    }
    case RequestType::Position: {
        if (request.playerId == 0 || !validSessionToken(request.sessionToken) || request.position.sequence == 0 ||
            !Net::isValidPlayerData(request.position.data)) {
            throw std::invalid_argument("invalid binary POSITION request");
        }
        Writer writer(MessageType::Position);
        writer.u32(request.playerId);
        writer.string(request.sessionToken);
        writer.u64(request.position.sequence);
        writer.f32(request.position.x);
        writer.f32(request.position.y);
        writer.string(request.position.data);
        return writer.finish();
    }
    case RequestType::Leave: {
        if (request.playerId == 0 || !validSessionToken(request.sessionToken)) {
            throw std::invalid_argument("invalid binary LEAVE request");
        }
        Writer writer(MessageType::Leave);
        writer.u32(request.playerId);
        writer.string(request.sessionToken);
        return writer.finish();
    }
    case RequestType::GetWorld:
        return Writer(MessageType::GetWorld).finish();
    }
    throw std::invalid_argument("unknown binary request type");
}

BinaryMessage encodeBinaryReply(const Reply& reply)
{
    switch (reply.type) {
    case ReplyType::Welcome: {
        if (reply.playerId == 0) {
            throw std::invalid_argument("invalid binary WELCOME reply");
        }
        Writer writer(MessageType::Welcome);
        writer.u32(reply.playerId);
        writer.string(reply.sessionEndpoint);
        writeSnapshot(writer, reply.snapshot);
        return writer.finish();
    }
    case ReplyType::Snapshot: {
        Writer writer(MessageType::Snapshot);
        writeSnapshot(writer, reply.snapshot);
        return writer.finish();
    }
    case ReplyType::WorldState: {
        Writer writer(MessageType::WorldState);
        writer.u64(reply.worldState.worldRevision);
        writePlatforms(writer, reply.worldState.platforms);
        return writer.finish();
    }
    case ReplyType::Error: {
        Writer writer(MessageType::Error);
        writer.string(reply.error);
        return writer.finish();
    }
    case ReplyType::Goodbye:
        return Writer(MessageType::Goodbye).finish();
    }
    throw std::invalid_argument("unknown binary reply type");
}

bool decodeBinaryRequest(const BinaryMessage& message, Request& request, std::string& error)
{
    Reader reader(message);
    MessageType type = MessageType::Join;
    if (!reader.header(type, error)) {
        return false;
    }

    request = {};
    switch (type) {
    case MessageType::Join:
        request.type = RequestType::Join;
        if (!reader.string(request.sessionToken, 32) || !validSessionToken(request.sessionToken) ||
            !reader.string(request.playerName, Net::maxNameLength) || !Net::isValidName(request.playerName)) {
            error = "invalid binary JOIN request";
            return false;
        }
        break;
    case MessageType::Position:
        request.type = RequestType::Position;
        if (!reader.u32(request.playerId) || request.playerId == 0 ||
            !reader.string(request.sessionToken, 32) || !validSessionToken(request.sessionToken) ||
            !reader.u64(request.position.sequence) || request.position.sequence == 0 ||
            !reader.f32(request.position.x) || !reader.f32(request.position.y) ||
            !reader.string(request.position.data, Net::maxPlayerDataLength)) {
            error = "invalid binary POSITION request";
            return false;
        }
        break;
    case MessageType::Leave:
        request.type = RequestType::Leave;
        if (!reader.u32(request.playerId) || request.playerId == 0 ||
            !reader.string(request.sessionToken, 32) || !validSessionToken(request.sessionToken)) {
            error = "invalid binary LEAVE request";
            return false;
        }
        break;
    case MessageType::GetWorld:
        request.type = RequestType::GetWorld;
        break;
    default:
        error = "binary message is not a request";
        return false;
    }
    return finish(reader, error);
}

bool decodeBinaryReply(const BinaryMessage& message, Reply& reply, std::string& error)
{
    Reader reader(message);
    MessageType type = MessageType::Error;
    if (!reader.header(type, error)) {
        return false;
    }

    reply = {};
    switch (type) {
    case MessageType::Welcome:
        reply.type = ReplyType::Welcome;
        if (!reader.u32(reply.playerId) || reply.playerId == 0 ||
            !reader.string(reply.sessionEndpoint, std::numeric_limits<std::uint16_t>::max()) ||
            !readSnapshot(reader, reply.snapshot, error)) {
            if (error.empty()) {
                error = "invalid binary WELCOME reply";
            }
            return false;
        }
        break;
    case MessageType::Snapshot:
        reply.type = ReplyType::Snapshot;
        if (!readSnapshot(reader, reply.snapshot, error)) {
            return false;
        }
        break;
    case MessageType::WorldState:
        reply.type = ReplyType::WorldState;
        if (!reader.u64(reply.worldState.worldRevision) ||
            !readPlatforms(reader, reply.worldState.platforms, error)) {
            if (error.empty()) {
                error = "invalid binary world state header";
            }
            return false;
        }
        break;
    case MessageType::Error:
        reply.type = ReplyType::Error;
        if (!reader.string(reply.error, std::numeric_limits<std::uint16_t>::max())) {
            error = "invalid binary ERROR reply";
            return false;
        }
        break;
    case MessageType::Goodbye:
        reply.type = ReplyType::Goodbye;
        break;
    default:
        error = "binary message is not a reply";
        return false;
    }
    return finish(reader, error);
}

} // namespace Network
