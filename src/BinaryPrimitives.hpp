#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

// Big-endian fixed-width values in a byte string, for codecs that append to or
// read from the middle of a message rather than owning a whole one.
//
// Private to the networking sources. Both the event codec and the binary
// client-server protocol (whose Writer/Reader wrap these) use them, so there
// is one copy of the conventions: big-endian integers, IEEE-754 bit patterns
// and u16-length strings.
//
// Every get* reads at `at`, advances it only on success, and returns false
// rather than reading past the end.
namespace Network::binary {

inline void putU8(std::string& out, std::uint8_t value)
{
    out.push_back(static_cast<char>(value));
}

inline void putU16(std::string& out, std::uint16_t value)
{
    putU8(out, static_cast<std::uint8_t>(value >> 8U));
    putU8(out, static_cast<std::uint8_t>(value));
}

inline void putU32(std::string& out, std::uint32_t value)
{
    putU16(out, static_cast<std::uint16_t>(value >> 16U));
    putU16(out, static_cast<std::uint16_t>(value));
}

inline void putU64(std::string& out, std::uint64_t value)
{
    putU32(out, static_cast<std::uint32_t>(value >> 32U));
    putU32(out, static_cast<std::uint32_t>(value));
}

// Signed values and doubles travel as their bit patterns. memcpy keeps that
// exact instead of relying on how a compiler converts negative numbers.
inline void putI32(std::string& out, std::int32_t value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(out, bits);
}

inline void putI64(std::string& out, std::int64_t value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    putU64(out, bits);
}

inline void putF64(std::string& out, double value)
{
    static_assert(sizeof(double) == sizeof(std::uint64_t), "64-bit doubles are required");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    putU64(out, bits);
}

inline void putString(std::string& out, const std::string& value)
{
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("binary string is too large");
    }
    putU16(out, static_cast<std::uint16_t>(value.size()));
    out.append(value);
}

inline bool getU8(const std::string& in, std::size_t& at, std::uint8_t& value)
{
    if (at >= in.size()) {
        return false;
    }
    value = static_cast<std::uint8_t>(static_cast<unsigned char>(in[at++]));
    return true;
}

inline bool getU16(const std::string& in, std::size_t& at, std::uint16_t& value)
{
    if (at > in.size() || in.size() - at < 2) {
        return false;
    }
    std::uint8_t high = 0;
    std::uint8_t low = 0;
    getU8(in, at, high);
    getU8(in, at, low);
    value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) | low);
    return true;
}

inline bool getU32(const std::string& in, std::size_t& at, std::uint32_t& value)
{
    if (at > in.size() || in.size() - at < 4) {
        return false;
    }
    std::uint16_t high = 0;
    std::uint16_t low = 0;
    getU16(in, at, high);
    getU16(in, at, low);
    value = (static_cast<std::uint32_t>(high) << 16U) | low;
    return true;
}

inline bool getU64(const std::string& in, std::size_t& at, std::uint64_t& value)
{
    if (at > in.size() || in.size() - at < 8) {
        return false;
    }
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    getU32(in, at, high);
    getU32(in, at, low);
    value = (static_cast<std::uint64_t>(high) << 32U) | low;
    return true;
}

inline bool getI32(const std::string& in, std::size_t& at, std::int32_t& value)
{
    std::uint32_t bits = 0;
    if (!getU32(in, at, bits)) {
        return false;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

inline bool getI64(const std::string& in, std::size_t& at, std::int64_t& value)
{
    std::uint64_t bits = 0;
    if (!getU64(in, at, bits)) {
        return false;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

inline bool getF64(const std::string& in, std::size_t& at, double& value)
{
    std::uint64_t bits = 0;
    if (!getU64(in, at, bits)) {
        return false;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

// Reads a u16-length string of at most `maximum` bytes. On failure `at` is
// unchanged, even if the length itself was readable.
inline bool getString(const std::string& in, std::size_t& at, std::string& value, std::size_t maximum)
{
    std::size_t next = at;
    std::uint16_t size = 0;
    if (!getU16(in, next, size) || size > maximum || in.size() - next < size) {
        return false;
    }
    value.assign(in, next, size);
    at = next + size;
    return true;
}

} // namespace Network::binary
