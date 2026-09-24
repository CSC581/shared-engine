#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

// Turning values into text fields and back, shared by every protocol in the
// engine.
//
// Both the client-server protocol and the peer-to-peer one put their messages
// on the wire as a list of text fields rather than as a struct, so that the
// wire never depends on C++ layout, padding or byte order. That decision is
// worth making once, and so is getting the parsing right: these functions have
// awkward corners in them (see parseFloat) that nobody should have to
// rediscover, and that a fix applied to one copy would silently miss in
// another.
//
// Deliberately free of ZeroMQ as well as of SDL. The representation of a
// message is a protocol question; how it is carried is a transport question,
// and the transport lives in ZmqMessage.hpp.
namespace Net {

// A message as it travels: one string per field. Both protocols use this, so
// a transport can carry either without knowing which it is holding.
using Frames = std::vector<std::string>;

// Strict digits-only parse: no sign, no whitespace, no leading plus, no
// partial consumption. Text off a network is not a place to be forgiving.
inline bool parseUnsigned(const std::string& text, std::uint64_t& value)
{
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        })) {
        return false;
    }
    try {
        std::size_t parsed = 0;
        value = std::stoull(text, &parsed);
        return parsed == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

// The same, allowing a single leading '-'. Rejects anything that would not fit
// an int64_t rather than wrapping it.
inline bool parseSigned(const std::string& text, std::int64_t& value)
{
    if (text.empty()) {
        return false;
    }
    const bool negative = text.front() == '-';
    std::uint64_t magnitude = 0;
    if (!parseUnsigned(negative ? text.substr(1) : text, magnitude) ||
        magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    value = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    return true;
}

// Parses a float written by formatFloat. Rejects infinities, NaN, trailing
// junk and anything that would not survive the narrowing, so a malformed or
// hostile field cannot put a non-finite value into a simulation.
inline bool parseFloat(const std::string& text, float& value)
{
    std::istringstream input(text);
    // The C locale explicitly: a machine whose locale writes decimal commas
    // must still read and write the same wire format as everybody else.
    input.imbue(std::locale::classic());
    // Parsing through double preserves subnormal floats on older libc++ builds.
    double parsed = 0.0;
    input >> std::noskipws >> parsed;
    if (input.fail() || !input.eof() || !std::isfinite(parsed) ||
        std::fabs(parsed) > std::numeric_limits<float>::max()) {
        return false;
    }
    value = static_cast<float>(parsed);
    // A non-zero value that narrowed to zero underflowed, and silently
    // becoming zero is exactly the kind of thing a position must not do.
    return parsed == 0.0 || value != 0.0F;
}

// Writes a float with enough digits to read back bit-identically.
inline std::string formatFloat(float value)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << static_cast<double>(value);
    return output.str();
}

// Limits on the text a game controls. These are not style rules: a name
// arrives from another machine and is drawn on a screen, so it is capped and
// restricted to printable ASCII rather than trusted to be reasonable.
constexpr std::size_t maxNameLength = 24;

// A game's own data about its player travels as one opaque field. ZeroMQ
// frames are length-delimited, so this can hold anything at all — text, packed
// binary, whatever the game likes — and the only thing the engine says about
// it is how big it may get.
constexpr std::size_t maxPlayerDataLength = 512;

// A display name. May be empty: not every game names its players.
inline bool isValidName(const std::string& name)
{
    return name.size() <= maxNameLength &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return c >= 0x20 && c < 0x7F;
           });
}

inline bool isValidPlayerData(const std::string& data)
{
    return data.size() <= maxPlayerDataLength;
}

} // namespace Net
