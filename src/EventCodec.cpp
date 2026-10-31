#include "EventCodec.hpp"

#include "BinaryPrimitives.hpp"
#include "WireFormat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace Network {
namespace {

static_assert(sizeof(int) == sizeof(std::int32_t), "Event::priority travels as 32 bits");

// Argument kinds on the wire. Spelled out rather than taken from the
// variant's index, so reordering EventValue can never change the format.
constexpr const char* textBool = "b";
constexpr const char* textInt = "i";
constexpr const char* textDouble = "d";
constexpr const char* textString = "s";

enum class Kind : std::uint8_t { Bool = 0, Int = 1, Double = 2, String = 3 };

// The smallest argument: a one-character name, its kind and a one-byte value
// (binary), or name, kind and value fields (text). Used to reject a count
// that cannot fit before anything is allocated for it.
constexpr std::size_t minimumArgBytes = 2 + 1 + 1 + 1;
constexpr std::size_t argFields = 3;

// What differs between the two directions.
template <class Entry>
struct Direction;

template <>
struct Direction<OutgoingEvent> {
    static constexpr bool carriesOrigin = false;
    // sequence, type, priority, timestamp, argument count.
    static constexpr std::size_t minimumFields = 5;
    // u64 sequence, u16 + 1 type, i32 priority, i64 timestamp, u8 count.
    static constexpr std::size_t minimumBytes = 8 + 2 + 1 + 4 + 8 + 1;

    static std::uint64_t sequence(const OutgoingEvent& entry) { return entry.clientSequence; }
    static void setSequence(OutgoingEvent& entry, std::uint64_t value) { entry.clientSequence = value; }
    // A client sends its own pending events in order, so a gap is a bug.
    static bool follows(std::uint64_t previous, std::uint64_t next) { return next == previous + 1; }
};

template <>
struct Direction<RelayedEvent> {
    static constexpr bool carriesOrigin = true;
    static constexpr std::size_t minimumFields = Direction<OutgoingEvent>::minimumFields + 1;
    static constexpr std::size_t minimumBytes = Direction<OutgoingEvent>::minimumBytes + 4;

    static std::uint64_t sequence(const RelayedEvent& entry) { return entry.serverSequence; }
    static void setSequence(RelayedEvent& entry, std::uint64_t value) { entry.serverSequence = value; }
    // A client never receives its own events, so the server's order skips.
    static bool follows(std::uint64_t previous, std::uint64_t next) { return next > previous; }
};

bool isValidName(const std::string& name)
{
    return !name.empty() && name.size() <= Net::maxEventNameLength &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
           });
}

// The value a decimal travels as. Subnormals (below about 2.2e-308) become
// zero of the same sign: some standard libraries cannot read them back from
// text, and a value decaying toward zero passes through that range, so
// sending zero keeps the formats in agreement without refusing the event.
double wireDecimal(double value)
{
    return std::fpclassify(value) == FP_SUBNORMAL ? std::copysign(0.0, value) : value;
}

// What a decoder accepts: exactly what an encoder can produce.
bool isWireDecimal(double value)
{
    return value == 0.0 || std::isnormal(value);
}

template <class Entry>
bool validateList(const std::vector<Entry>& events, std::string& error)
{
    if (events.size() > Net::maxEventsPerMessage) {
        error = "too many events in one message";
        return false;
    }
    std::uint64_t previous = 0;
    for (const Entry& entry : events) {
        const std::uint64_t sequence = Direction<Entry>::sequence(entry);
        if (sequence == 0 || (previous != 0 && !Direction<Entry>::follows(previous, sequence))) {
            error = "event sequence numbers are out of order";
            return false;
        }
        // Origin 0 means "this machine": a relayed event marked that way would
        // be taken for a local one and sent straight back out.
        if (Direction<Entry>::carriesOrigin && entry.event.origin == 0) {
            error = "a relayed event needs an origin";
            return false;
        }
        if (!validateEvent(entry.event, error)) {
            return false;
        }
        previous = sequence;
    }
    return true;
}

// --- Text --------------------------------------------------------------------

void appendTextValue(Message& out, const EventValue& value)
{
    if (const auto* flag = std::get_if<bool>(&value)) {
        out.push_back(textBool);
        out.push_back(*flag ? "1" : "0");
    } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        out.push_back(textInt);
        out.push_back(std::to_string(*integer));
    } else if (const auto* decimal = std::get_if<double>(&value)) {
        out.push_back(textDouble);
        out.push_back(Net::formatDouble(wireDecimal(*decimal)));
    } else {
        out.push_back(textString);
        out.push_back(std::get<std::string>(value));
    }
}

template <class Entry>
void appendText(Message& out, const std::vector<Entry>& events)
{
    std::string error;
    if (!validateList(events, error)) {
        throw std::invalid_argument(error);
    }
    out.push_back(std::to_string(events.size()));
    for (const Entry& entry : events) {
        const Event& event = entry.event;
        out.push_back(std::to_string(Direction<Entry>::sequence(entry)));
        out.push_back(event.type);
        out.push_back(std::to_string(event.priority));
        if (Direction<Entry>::carriesOrigin) {
            out.push_back(std::to_string(event.origin));
        }
        out.push_back(std::to_string(event.timestamp));
        out.push_back(std::to_string(event.args.size()));
        for (const auto& [name, value] : event.args) {
            out.push_back(name);
            appendTextValue(out, value);
        }
    }
}

// The field at `at`, advancing past it, or nullptr at the end of the message.
const std::string* nextField(const Message& in, std::size_t& at)
{
    return at < in.size() ? &in[at++] : nullptr;
}

bool readTextValue(const std::string& kind, const std::string& text, EventValue& value)
{
    if (kind == textBool) {
        if (text != "0" && text != "1") {
            return false;
        }
        value = text == "1";
        return true;
    }
    if (kind == textInt) {
        std::int64_t integer = 0;
        if (!Net::parseSigned(text, integer)) {
            return false;
        }
        value = integer;
        return true;
    }
    if (kind == textDouble) {
        double decimal = 0.0;
        if (!Net::parseDouble(text, decimal)) {
            return false;
        }
        value = decimal;
        return true;
    }
    if (kind == textString) {
        value = text;
        return true;
    }
    return false;
}

template <class Entry>
bool readTextEvent(const Message& in, std::size_t& at, Entry& entry, std::string& error)
{
    Event& event = entry.event;
    std::uint64_t sequence = 0;
    std::int64_t priority = 0;
    std::uint64_t origin = 0;
    std::uint64_t argCount = 0;

    const std::string* field = nextField(in, at);
    if (field == nullptr || !Net::parseUnsigned(*field, sequence)) {
        error = "invalid event sequence";
        return false;
    }
    field = nextField(in, at);
    if (field == nullptr) {
        error = "missing event type";
        return false;
    }
    event.type = *field;
    field = nextField(in, at);
    if (field == nullptr || !Net::parseSigned(*field, priority) ||
        priority < std::numeric_limits<std::int32_t>::min() || priority > std::numeric_limits<std::int32_t>::max()) {
        error = "invalid event priority";
        return false;
    }
    if (Direction<Entry>::carriesOrigin) {
        field = nextField(in, at);
        if (field == nullptr || !Net::parseUnsigned(*field, origin) ||
            origin > std::numeric_limits<std::uint32_t>::max()) {
            error = "invalid event origin";
            return false;
        }
    }
    field = nextField(in, at);
    if (field == nullptr || !Net::parseSigned(*field, event.timestamp)) {
        error = "invalid event timestamp";
        return false;
    }
    field = nextField(in, at);
    if (field == nullptr || !Net::parseUnsigned(*field, argCount) || argCount > Net::maxEventArgs ||
        argCount > (in.size() - at) / argFields) {
        error = "invalid event argument count";
        return false;
    }

    for (std::uint64_t i = 0; i < argCount; ++i) {
        const std::string& name = in[at];
        EventValue value;
        if (!readTextValue(in[at + 1], in[at + 2], value)) {
            error = "invalid event argument";
            return false;
        }
        at += argFields;
        if (!event.args.emplace(name, std::move(value)).second) {
            error = "duplicate event argument name";
            return false;
        }
    }

    Direction<Entry>::setSequence(entry, sequence);
    event.priority = static_cast<int>(priority);
    event.origin = static_cast<std::uint32_t>(origin);
    return true;
}

template <class Entry>
bool readText(const Message& in, std::size_t& cursor, std::vector<Entry>& out, std::string& error)
{
    std::size_t at = cursor;
    std::uint64_t count = 0;
    const std::string* field = nextField(in, at);
    if (field == nullptr || !Net::parseUnsigned(*field, count) || count > Net::maxEventsPerMessage ||
        count > (in.size() - at) / Direction<Entry>::minimumFields) {
        error = "invalid event count";
        return false;
    }

    std::vector<Entry> events(static_cast<std::size_t>(count));
    for (Entry& entry : events) {
        if (!readTextEvent(in, at, entry, error)) {
            return false;
        }
    }
    if (!validateList(events, error)) {
        return false;
    }
    out = std::move(events);
    cursor = at;
    return true;
}

// --- Binary ------------------------------------------------------------------

void appendBinaryValue(std::string& out, const EventValue& value)
{
    if (const auto* flag = std::get_if<bool>(&value)) {
        binary::putU8(out, static_cast<std::uint8_t>(Kind::Bool));
        binary::putU8(out, *flag ? 1U : 0U);
    } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        binary::putU8(out, static_cast<std::uint8_t>(Kind::Int));
        binary::putI64(out, *integer);
    } else if (const auto* decimal = std::get_if<double>(&value)) {
        binary::putU8(out, static_cast<std::uint8_t>(Kind::Double));
        binary::putF64(out, wireDecimal(*decimal));
    } else {
        binary::putU8(out, static_cast<std::uint8_t>(Kind::String));
        binary::putString(out, std::get<std::string>(value));
    }
}

template <class Entry>
void appendBinary(std::string& out, const std::vector<Entry>& events)
{
    std::string error;
    if (!validateList(events, error)) {
        throw std::invalid_argument(error);
    }
    binary::putU16(out, static_cast<std::uint16_t>(events.size()));
    for (const Entry& entry : events) {
        const Event& event = entry.event;
        binary::putU64(out, Direction<Entry>::sequence(entry));
        binary::putString(out, event.type);
        binary::putI32(out, event.priority);
        if (Direction<Entry>::carriesOrigin) {
            binary::putU32(out, event.origin);
        }
        binary::putI64(out, event.timestamp);
        binary::putU8(out, static_cast<std::uint8_t>(event.args.size()));
        for (const auto& [name, value] : event.args) {
            binary::putString(out, name);
            appendBinaryValue(out, value);
        }
    }
}

bool readBinaryValue(const std::string& in, std::size_t& at, EventValue& value)
{
    std::uint8_t kind = 0;
    if (!binary::getU8(in, at, kind)) {
        return false;
    }
    switch (static_cast<Kind>(kind)) {
    case Kind::Bool: {
        std::uint8_t flag = 0;
        if (!binary::getU8(in, at, flag) || flag > 1U) {
            return false;
        }
        value = flag == 1U;
        return true;
    }
    case Kind::Int: {
        std::int64_t integer = 0;
        if (!binary::getI64(in, at, integer)) {
            return false;
        }
        value = integer;
        return true;
    }
    case Kind::Double: {
        double decimal = 0.0;
        // NaN, infinities and subnormals never come from an encoder.
        if (!binary::getF64(in, at, decimal) || !isWireDecimal(decimal)) {
            return false;
        }
        value = decimal;
        return true;
    }
    case Kind::String: {
        std::string text;
        if (!binary::getString(in, at, text, Net::maxEventTextLength)) {
            return false;
        }
        value = std::move(text);
        return true;
    }
    }
    return false;
}

template <class Entry>
bool readBinaryEvent(const std::string& in, std::size_t& at, Entry& entry, std::string& error)
{
    Event& event = entry.event;
    std::uint64_t sequence = 0;
    std::int32_t priority = 0;
    std::uint8_t argCount = 0;

    if (!binary::getU64(in, at, sequence) || !binary::getString(in, at, event.type, Net::maxEventNameLength) ||
        !binary::getI32(in, at, priority) ||
        (Direction<Entry>::carriesOrigin && !binary::getU32(in, at, event.origin)) ||
        !binary::getI64(in, at, event.timestamp) || !binary::getU8(in, at, argCount)) {
        error = "invalid or truncated event";
        return false;
    }
    if (argCount > Net::maxEventArgs || argCount > (in.size() - at) / minimumArgBytes) {
        error = "invalid event argument count";
        return false;
    }

    for (std::uint8_t i = 0; i < argCount; ++i) {
        std::string name;
        EventValue value;
        if (!binary::getString(in, at, name, Net::maxEventNameLength) || !readBinaryValue(in, at, value)) {
            error = "invalid event argument";
            return false;
        }
        if (!event.args.emplace(std::move(name), std::move(value)).second) {
            error = "duplicate event argument name";
            return false;
        }
    }

    Direction<Entry>::setSequence(entry, sequence);
    event.priority = priority;
    return true;
}

template <class Entry>
bool readBinary(const std::string& in, std::size_t& cursor, std::vector<Entry>& out, std::string& error)
{
    std::size_t at = cursor;
    std::uint16_t count = 0;
    if (!binary::getU16(in, at, count) || count > Net::maxEventsPerMessage ||
        count > (in.size() - at) / Direction<Entry>::minimumBytes) {
        error = "invalid event count";
        return false;
    }

    std::vector<Entry> events(count);
    for (Entry& entry : events) {
        if (!readBinaryEvent(in, at, entry, error)) {
            return false;
        }
    }
    if (!validateList(events, error)) {
        return false;
    }
    out = std::move(events);
    cursor = at;
    return true;
}

} // namespace

bool validateEvent(const Event& event, std::string& error)
{
    if (!isValidName(event.type)) {
        error = "invalid event type";
        return false;
    }
    if (event.timestamp < 0) {
        error = "event timestamp is negative";
        return false;
    }
    if (event.args.size() > Net::maxEventArgs) {
        error = "too many event arguments";
        return false;
    }
    for (const auto& [name, value] : event.args) {
        if (!isValidName(name)) {
            error = "invalid event argument name";
            return false;
        }
        const auto* decimal = std::get_if<double>(&value);
        if (decimal != nullptr && !std::isfinite(*decimal)) {
            error = "event decimal must be finite";
            return false;
        }
        const auto* text = std::get_if<std::string>(&value);
        if (text != nullptr && text->size() > Net::maxEventTextLength) {
            error = "event text value is too long";
            return false;
        }
    }
    return true;
}

void appendEventsText(Message& out, const std::vector<OutgoingEvent>& events)
{
    appendText(out, events);
}

void appendEventsText(Message& out, const std::vector<RelayedEvent>& events)
{
    appendText(out, events);
}

void appendEventsBinary(std::string& out, const std::vector<OutgoingEvent>& events)
{
    appendBinary(out, events);
}

void appendEventsBinary(std::string& out, const std::vector<RelayedEvent>& events)
{
    appendBinary(out, events);
}

bool readEventsText(const Message& in, std::size_t& cursor, std::vector<OutgoingEvent>& out, std::string& error)
{
    return readText(in, cursor, out, error);
}

bool readEventsText(const Message& in, std::size_t& cursor, std::vector<RelayedEvent>& out, std::string& error)
{
    return readText(in, cursor, out, error);
}

bool readEventsBinary(const std::string& in, std::size_t& cursor, std::vector<OutgoingEvent>& out,
                      std::string& error)
{
    return readBinary(in, cursor, out, error);
}

bool readEventsBinary(const std::string& in, std::size_t& cursor, std::vector<RelayedEvent>& out,
                      std::string& error)
{
    return readBinary(in, cursor, out, error);
}

} // namespace Network
