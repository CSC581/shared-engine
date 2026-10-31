// Event lists in the text and binary formats: exact round trips, the shared
// validation rules, and decoding of hand-written, truncated and corrupted
// input. Every check runs on both formats unless it is format-specific.
#include "EventCodec.hpp"
#include "WireFormat.hpp"

#include "../src/BinaryPrimitives.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <locale>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what, int line)
{
    if (!ok) {
        std::cerr << "line " << line << ": " << what << '\n';
        ++failures;
    }
}
#define CHECK(x) check((x), #x, __LINE__)

using Network::Message;
using Network::OutgoingEvent;
using Network::RelayedEvent;

// Doubles compare by bit pattern, so 0.0 and -0.0 count as different.
bool sameValue(const EventValue& a, const EventValue& b)
{
    if (a.index() != b.index()) {
        return false;
    }
    if (const auto* x = std::get_if<double>(&a)) {
        const double y = std::get<double>(b);
        return std::memcmp(x, &y, sizeof(double)) == 0;
    }
    return a == b;
}

bool sameEvent(const Event& a, const Event& b)
{
    if (a.type != b.type || a.priority != b.priority || a.timestamp != b.timestamp || a.origin != b.origin ||
        a.args.size() != b.args.size()) {
        return false;
    }
    for (const auto& [name, value] : a.args) {
        const auto it = b.args.find(name);
        if (it == b.args.end() || !sameValue(value, it->second)) {
            return false;
        }
    }
    return true;
}

std::uint64_t sequenceOf(const OutgoingEvent& entry) { return entry.clientSequence; }
std::uint64_t sequenceOf(const RelayedEvent& entry) { return entry.serverSequence; }

// What a receiver should get back: everything that travels, and 0 for what
// does not. A client event's origin never travels.
Event expectedAfterTrip(Event event, bool carriesOrigin)
{
    event.deliverAt = 0;
    event.sequence = 0;
    if (!carriesOrigin) {
        event.origin = 0;
    }
    return event;
}

// Encode between unrelated fields, decode from the middle, and check that the
// cursor stops exactly at what follows.
template <class Entry>
std::vector<Entry> textTrip(const std::vector<Entry>& events)
{
    Message message{"before"};
    Network::appendEventsText(message, events);
    message.push_back("after");
    std::size_t cursor = 1;
    std::vector<Entry> decoded;
    std::string error;
    CHECK(Network::readEventsText(message, cursor, decoded, error));
    CHECK(cursor == message.size() - 1);
    return decoded;
}

template <class Entry>
std::vector<Entry> binaryTrip(const std::vector<Entry>& events)
{
    std::string bytes = "AB";
    Network::appendEventsBinary(bytes, events);
    bytes += 'Z';
    std::size_t cursor = 2;
    std::vector<Entry> decoded;
    std::string error;
    CHECK(Network::readEventsBinary(bytes, cursor, decoded, error));
    CHECK(cursor == bytes.size() - 1);
    return decoded;
}

template <class Entry>
bool tripMatches(const std::vector<Entry>& original, const std::vector<Entry>& decoded, bool carriesOrigin)
{
    if (original.size() != decoded.size()) {
        return false;
    }
    for (std::size_t i = 0; i < original.size(); ++i) {
        const Event& got = decoded[i].event;
        if (sequenceOf(original[i]) != sequenceOf(decoded[i]) || got.deliverAt != 0 || got.sequence != 0 ||
            !sameEvent(expectedAfterTrip(original[i].event, carriesOrigin), got)) {
            return false;
        }
    }
    return true;
}

template <class Entry>
bool roundTrips(const std::vector<Entry>& events, bool carriesOrigin)
{
    return tripMatches(events, textTrip(events), carriesOrigin) &&
           tripMatches(events, binaryTrip(events), carriesOrigin);
}

bool roundTripsBothWays(const Event& event)
{
    Event relayed = event;
    if (relayed.origin == 0) {
        relayed.origin = 3;
    }
    return roundTrips(std::vector<OutgoingEvent>{{1, event}}, false) &&
           roundTrips(std::vector<RelayedEvent>{{1, relayed}}, true);
}

// --- Decimal text ------------------------------------------------------------

void decimalText()
{
    for (const double value : {0.0, -0.0, 0.1, -2.5, 1e300, -1e-300, 16777217.0,
                               std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
                               std::numeric_limits<double>::min(), std::nextafter(1.0, 2.0)}) {
        double parsed = 1.0;
        CHECK(Net::parseDouble(Net::formatDouble(value), parsed));
        CHECK(std::memcmp(&parsed, &value, sizeof(double)) == 0);
    }

    double parsed = 7.0;
    for (const char* text : {"nan", "inf", "-inf", "1e999", "1e-400", "4.9e-324", "+1", " 1", "1 ", "1,5",
                             "0x1p3", "", "-", ".", "1e", "1e+", "--1", "1.2.3"}) {
        CHECK(!Net::parseDouble(text, parsed));
    }
    CHECK(parsed == 7.0);
    CHECK(Net::parseDouble("-0", parsed) && parsed == 0.0 && std::signbit(parsed));
    CHECK(Net::parseDouble("0.000e5", parsed) && parsed == 0.0);

    // The whole int64 range, and nothing past either end.
    std::int64_t integer = 0;
    CHECK(Net::parseSigned("-9223372036854775808", integer) && integer == std::numeric_limits<std::int64_t>::min());
    CHECK(Net::parseSigned("9223372036854775807", integer) && integer == std::numeric_limits<std::int64_t>::max());
    CHECK(!Net::parseSigned("-9223372036854775809", integer) && !Net::parseSigned("9223372036854775808", integer));
}

// --- Round trips -------------------------------------------------------------

void valuesAndFields()
{
    Event integers{"Counts"};
    integers.set("zero", std::int64_t{0}).set("minusOne", std::int64_t{-1})
            .set("min", std::numeric_limits<std::int64_t>::min())
            .set("max", std::numeric_limits<std::int64_t>::max())
            .set("yes", true).set("no", false);
    CHECK(roundTripsBothWays(integers));

    Event decimals{"Decimals"};
    decimals.set("zero", 0.0).set("negativeZero", -0.0).set("tenth", 0.1).set("huge", 1e300)
            .set("tiny", -1e-300).set("notAFloat", 16777217.0)
            .set("smallestNormal", std::numeric_limits<double>::min());
    CHECK(roundTripsBothWays(decimals));

    Event texts{"Texts"};
    texts.set("empty", "").set("full", std::string(Net::maxEventTextLength, 'x'))
         .set("spaces", "a b  c").set("utf8", "caf\xC3\xA9").set("nul", std::string("a\0b", 3));
    CHECK(roundTripsBothWays(texts));

    // Header fields at their edges.
    for (const int priority : {EventPriority::High, EventPriority::Low, std::numeric_limits<int>::min(),
                               std::numeric_limits<int>::max()}) {
        Event event{"Prioritised"};
        event.priority = priority;
        event.timestamp = std::numeric_limits<std::int64_t>::max();
        CHECK(roundTripsBothWays(event));
    }
    Event fromServer{"Server.Event_1-x"};
    fromServer.origin = Events::ServerOrigin;
    CHECK(roundTrips(std::vector<RelayedEvent>{{9, fromServer}}, true));

    // Local fields never travel; a client event's origin never travels.
    Event local{"Local"};
    local.deliverAt = 500;
    local.sequence = 42;
    local.origin = 7;
    const auto text = textTrip(std::vector<OutgoingEvent>{{1, local}});
    const auto bytes = binaryTrip(std::vector<OutgoingEvent>{{1, local}});
    CHECK(text.size() == 1 && text[0].event.deliverAt == 0 && text[0].event.sequence == 0 &&
          text[0].event.origin == 0);
    CHECK(bytes.size() == 1 && bytes[0].event.deliverAt == 0 && bytes[0].event.sequence == 0 &&
          bytes[0].event.origin == 0);

    // The text layout, field by field, for one relayed event.
    Event death{"DeathEvent"};
    death.origin = 3;
    death.timestamp = 250000;
    death.set("object", std::int64_t{12}).set("cause", "deathzone");
    Message message;
    Network::appendEventsText(message, std::vector<RelayedEvent>{{17, death}});
    CHECK((message == Message{"1", "17", "DeathEvent", "0", "3", "250000", "2", "cause", "s", "deathzone",
                              "object", "i", "12"}));
}

// A value decaying toward zero passes through the subnormal range. It is
// still sent, as zero of the same sign, in both formats.
void subnormalsTravelAsZero()
{
    Event tiny{"Decaying"};
    tiny.set("positive", std::numeric_limits<double>::denorm_min())
        .set("negative", -std::numeric_limits<double>::min() / 4.0);
    std::string error;
    CHECK(Network::validateEvent(tiny, error));

    const auto zeroOfSign = [](const Event& event, const char* name, bool negative) {
        const double value = event.get<double>(name);
        return value == 0.0 && std::signbit(value) == negative;
    };
    const auto text = textTrip(std::vector<OutgoingEvent>{{1, tiny}});
    const auto bytes = binaryTrip(std::vector<OutgoingEvent>{{1, tiny}});
    CHECK(text.size() == 1 && zeroOfSign(text[0].event, "positive", false) &&
          zeroOfSign(text[0].event, "negative", true));
    CHECK(bytes.size() == 1 && zeroOfSign(bytes[0].event, "positive", false) &&
          zeroOfSign(bytes[0].event, "negative", true));
}

void lists()
{
    CHECK(roundTrips(std::vector<OutgoingEvent>{}, false));
    CHECK(roundTrips(std::vector<RelayedEvent>{}, true));

    std::vector<OutgoingEvent> outgoing;
    std::vector<RelayedEvent> relayed;
    for (std::size_t i = 0; i < Net::maxEventsPerMessage; ++i) {
        Event event{"Burst"};
        event.set("index", static_cast<std::int64_t>(i));
        outgoing.push_back({5 + i, event});
        event.origin = 2;
        relayed.push_back({10 + 3 * i, event});
    }
    CHECK(roundTrips(outgoing, false));
    CHECK(roundTrips(relayed, true));
}

struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};

// A machine set to decimal commas must write and read the same text.
void locale()
{
    Event event{"Measured"};
    event.set("value", 1234.5);
    const std::locale saved = std::locale();
    std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    Message message;
    Network::appendEventsText(message, std::vector<OutgoingEvent>{{1, event}});
    std::locale::global(saved);
    CHECK(message.back() == "1234.5");

    std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    std::size_t cursor = 0;
    std::vector<OutgoingEvent> decoded;
    std::string error;
    CHECK(Network::readEventsText(message, cursor, decoded, error) &&
          decoded[0].event.get<double>("value") == 1234.5);
    std::locale::global(saved);
}

std::string randomName(std::mt19937& rng)
{
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-";
    std::uniform_int_distribution<std::size_t> length(1, 12);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::string name(length(rng), ' ');
    for (char& c : name) {
        c = alphabet[pick(rng)];
    }
    return name;
}

Event randomEvent(std::mt19937& rng)
{
    Event event{randomName(rng)};
    event.priority = std::uniform_int_distribution<int>(std::numeric_limits<int>::min(),
                                                        std::numeric_limits<int>::max())(rng);
    event.timestamp = std::uniform_int_distribution<std::int64_t>(0, std::numeric_limits<std::int64_t>::max())(rng);
    const std::size_t argCount = std::uniform_int_distribution<std::size_t>(0, Net::maxEventArgs)(rng);
    while (event.args.size() < argCount) {
        const std::string name = randomName(rng);
        switch (std::uniform_int_distribution<int>(0, 3)(rng)) {
        case 0:
            event.set(name, std::uniform_int_distribution<int>(0, 1)(rng) == 1);
            break;
        case 1:
            event.set(name, std::uniform_int_distribution<std::int64_t>(std::numeric_limits<std::int64_t>::min(),
                                                                        std::numeric_limits<std::int64_t>::max())(rng));
            break;
        case 2:
            event.set(name, std::uniform_real_distribution<double>(-1e9, 1e9)(rng));
            break;
        default: {
            std::string text(std::uniform_int_distribution<std::size_t>(0, 40)(rng), '\0');
            for (char& c : text) {
                c = static_cast<char>(std::uniform_int_distribution<int>(0, 255)(rng));
            }
            event.set(name, text);
        }
        }
    }
    return event;
}

// Text must carry every normal double exactly, not just everyday values:
// random bit patterns cover the whole range, tiny to huge, in both formats.
void decimalsAcrossTheRange()
{
    std::mt19937_64 rng(1);
    int checked = 0;
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t bits = rng();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isnormal(value)) {
            continue;
        }
        double parsed = 0.0;
        if (!Net::parseDouble(Net::formatDouble(value), parsed) || std::memcmp(&parsed, &value, sizeof(double)) != 0) {
            CHECK(!"a normal double did not survive text exactly");
            return;
        }
        ++checked;
    }
    CHECK(checked > 90000);

    Event powers{"Powers"};
    for (int exponent = -307; exponent <= 308; exponent += 41) {
        powers.set("e" + std::to_string(exponent + 400), std::pow(10.0, exponent));
    }
    CHECK(roundTripsBothWays(powers));
}

void randomRoundTrips()
{
    std::mt19937 rng(581);
    for (int i = 0; i < 1000; ++i) {
        Event event = randomEvent(rng);
        if (!roundTripsBothWays(event)) {
            CHECK(!"random event did not round trip");
            return;
        }
    }
}

// --- Rejections --------------------------------------------------------------

// Every encoder refuses the event, and the output it was given is unchanged.
bool allEncodersRefuse(const Event& event)
{
    std::string error;
    bool refused = !Network::validateEvent(event, error) && !error.empty();
    Event relayed = event;
    relayed.origin = relayed.origin == 0 ? 3 : relayed.origin;
    const auto refuses = [](const std::function<void()>& encode) {
        try {
            encode();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    Message message{"keep"};
    std::string bytes = "keep";
    refused &= refuses([&] { Network::appendEventsText(message, std::vector<OutgoingEvent>{{1, event}}); });
    refused &= refuses([&] { Network::appendEventsText(message, std::vector<RelayedEvent>{{1, relayed}}); });
    refused &= refuses([&] { Network::appendEventsBinary(bytes, std::vector<OutgoingEvent>{{1, event}}); });
    refused &= refuses([&] { Network::appendEventsBinary(bytes, std::vector<RelayedEvent>{{1, relayed}}); });
    return refused && message == Message{"keep"} && bytes == "keep";
}

void rejectsInvalidEvents()
{
    std::vector<Event> invalid;
    invalid.emplace_back("");
    invalid.emplace_back(std::string(Net::maxEventNameLength + 1, 'a'));
    invalid.emplace_back("Has Space");
    invalid.emplace_back("Bad\nType");
    invalid.emplace_back("Plus+");
    Event negative{"Negative"};
    negative.timestamp = -1;
    invalid.push_back(negative);
    Event tooMany{"TooMany"};
    for (std::size_t i = 0; i <= Net::maxEventArgs; ++i) {
        tooMany.set("a" + std::to_string(i), true);
    }
    invalid.push_back(tooMany);
    invalid.push_back(Event{"BadName"}.set("", true));
    invalid.push_back(Event{"BadName"}.set("with space", true));
    invalid.push_back(Event{"BadName"}.set(std::string(Net::maxEventNameLength + 1, 'n'), true));
    invalid.push_back(Event{"LongText"}.set("text", std::string(Net::maxEventTextLength + 1, 'x')));
    for (const double bad : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        invalid.push_back(Event{"BadDecimal"}.set("value", bad));
    }
    for (const Event& event : invalid) {
        CHECK(allEncodersRefuse(event));
    }

    // The limits themselves are allowed.
    Event atLimits{std::string(Net::maxEventNameLength, 'T')};
    for (std::size_t i = 0; i < Net::maxEventArgs; ++i) {
        atLimits.set(std::string(Net::maxEventNameLength - 2, 'n') + std::to_string(10 + i),
                     std::string(Net::maxEventTextLength, 'v'));
    }
    CHECK(roundTripsBothWays(atLimits));
}

template <class Entry>
bool listRefused(const std::vector<Entry>& events)
{
    Message message{"keep"};
    std::string bytes = "keep";
    bool textThrew = false;
    bool binaryThrew = false;
    try { Network::appendEventsText(message, events); } catch (const std::invalid_argument&) { textThrew = true; }
    try { Network::appendEventsBinary(bytes, events); } catch (const std::invalid_argument&) { binaryThrew = true; }
    return textThrew && binaryThrew && message == Message{"keep"} && bytes == "keep";
}

void rejectsInvalidLists()
{
    const Event plain{"Plain"};
    Event withOrigin{"Plain"};
    withOrigin.origin = 4;

    CHECK(listRefused(std::vector<OutgoingEvent>(Net::maxEventsPerMessage + 1, OutgoingEvent{1, plain})));
    CHECK(listRefused(std::vector<OutgoingEvent>{{0, plain}}));
    CHECK(listRefused(std::vector<OutgoingEvent>{{1, plain}, {3, plain}}));
    CHECK(listRefused(std::vector<OutgoingEvent>{{2, plain}, {1, plain}}));
    CHECK(listRefused(std::vector<OutgoingEvent>{{1, plain}, {1, plain}}));
    CHECK(listRefused(std::vector<RelayedEvent>{{0, withOrigin}}));
    CHECK(listRefused(std::vector<RelayedEvent>{{5, withOrigin}, {5, withOrigin}}));
    CHECK(listRefused(std::vector<RelayedEvent>{{5, withOrigin}, {4, withOrigin}}));
    CHECK(listRefused(std::vector<RelayedEvent>{{1, plain}}));   // origin 0
}

// A decoder that refuses must leave its output and cursor alone.
template <class Entry>
bool textRefused(const Message& message)
{
    std::vector<Entry> out(1);
    std::size_t cursor = 0;
    std::string error;
    return !Network::readEventsText(message, cursor, out, error) && !error.empty() && cursor == 0 &&
           out.size() == 1 && sequenceOf(out[0]) == 0;
}

template <class Entry>
bool binaryRefused(const std::string& bytes)
{
    std::vector<Entry> out(1);
    std::size_t cursor = 0;
    std::string error;
    return !Network::readEventsBinary(bytes, cursor, out, error) && !error.empty() && cursor == 0 &&
           out.size() == 1 && sequenceOf(out[0]) == 0;
}

// Rules only a hand-written or hostile message can break.
void decoderRejectsHandWrittenText()
{
    // One relayed event: count, sequence, type, priority, origin, timestamp,
    // argument count, then name, kind, value.
    const Message valid{"1", "1", "Hit", "0", "7", "0", "1", "n", "i", "5"};
    CHECK(textRefused<RelayedEvent>({}));
    std::vector<RelayedEvent> decoded;
    std::size_t cursor = 0;
    std::string error;
    CHECK(Network::readEventsText(valid, cursor, decoded, error));

    const auto withField = [&valid](std::size_t index, const std::string& value) {
        Message message = valid;
        message[index] = value;
        return message;
    };
    CHECK(textRefused<RelayedEvent>(withField(0, "4000000000")));                  // count far past the message
    CHECK(textRefused<RelayedEvent>(withField(0, "65")));                          // past the limit
    CHECK(textRefused<RelayedEvent>(withField(0, "-1")));
    CHECK(textRefused<RelayedEvent>(withField(1, "0")));                           // sequence 0
    CHECK(textRefused<RelayedEvent>(withField(2, "Bad Type")));
    CHECK(textRefused<RelayedEvent>(withField(3, "2147483648")));                  // priority past 32 bits
    CHECK(textRefused<RelayedEvent>(withField(3, "-2147483649")));
    CHECK(textRefused<RelayedEvent>(withField(4, "4294967296")));                  // origin past 32 bits
    CHECK(textRefused<RelayedEvent>(withField(4, "0")));                           // origin "this machine"
    CHECK(textRefused<RelayedEvent>(withField(5, "-1")));                          // negative timestamp
    CHECK(textRefused<RelayedEvent>(withField(6, "17")));                          // too many arguments
    CHECK(textRefused<RelayedEvent>(withField(6, "2")));                           // more than are present
    CHECK(textRefused<RelayedEvent>(withField(7, "")));                            // empty argument name
    CHECK(textRefused<RelayedEvent>(withField(8, "x")));                           // unknown kind
    CHECK(textRefused<RelayedEvent>(withField(9, "9223372036854775808")));         // int past 64 bits
    CHECK(textRefused<RelayedEvent>(withField(9, "-9223372036854775809")));
    CHECK(textRefused<RelayedEvent>(Message{"1", "1", "Hit", "0", "7", "0", "1", "n", "b", "2"}));
    CHECK(textRefused<RelayedEvent>(Message{"1", "1", "Hit", "0", "7", "0", "1", "n", "b", "true"}));
    for (const char* decimal : {"nan", "inf", "1e999", "1e-400", "4.9e-324", "+1", "1,5", "0x1p3", ""}) {
        CHECK(textRefused<RelayedEvent>(Message{"1", "1", "Hit", "0", "7", "0", "1", "n", "d", decimal}));
    }
    CHECK(textRefused<RelayedEvent>(Message{"1", "1", "Hit", "0", "7", "0", "1", "n", "s",
                                            std::string(Net::maxEventTextLength + 1, 'x')}));
    CHECK(textRefused<RelayedEvent>(Message{"1", "1", "Hit", "0", "7", "0", "2", "n", "i", "1", "n", "i", "2"}));
    // Two well-formed outgoing events (no origin field) numbered 1 then 3:
    // client sequences must be consecutive.
    CHECK(textRefused<OutgoingEvent>(Message{"2", "1", "A", "0", "0", "0", "3", "B", "0", "0", "0"}));
}

// A relayed event written field by field, for the binary decoder.
struct BinaryEvent {
    std::uint16_t count = 1;
    std::uint64_t sequence = 1;
    std::string type = "Hit";
    std::int32_t priority = 0;
    std::uint32_t origin = 7;
    std::int64_t timestamp = 0;
    std::uint8_t argCount = 0;
    std::string argBytes;

    std::string bytes() const
    {
        std::string out;
        Network::binary::putU16(out, count);
        Network::binary::putU64(out, sequence);
        Network::binary::putString(out, type);
        Network::binary::putI32(out, priority);
        Network::binary::putU32(out, origin);
        Network::binary::putI64(out, timestamp);
        Network::binary::putU8(out, argCount);
        return out + argBytes;
    }
};

std::string binaryArg(const std::string& name, std::uint8_t kind, const std::string& valueBytes)
{
    std::string out;
    Network::binary::putString(out, name);
    Network::binary::putU8(out, kind);
    return out + valueBytes;
}

std::string f64Bytes(double value)
{
    std::string out;
    Network::binary::putF64(out, value);
    return out;
}

void decoderRejectsHandWrittenBinary()
{
    BinaryEvent valid;
    valid.argCount = 1;
    valid.argBytes = binaryArg("n", 2, f64Bytes(1.5));
    std::vector<RelayedEvent> decoded;
    std::size_t cursor = 0;
    std::string error;
    CHECK(Network::readEventsBinary(valid.bytes(), cursor, decoded, error));

    const auto with = [&valid](const std::function<void(BinaryEvent&)>& change) {
        BinaryEvent event = valid;
        change(event);
        return event.bytes();
    };
    CHECK(binaryRefused<RelayedEvent>(""));
    {
        // A start position past the end is refused, not read from.
        const std::string bytes = valid.bytes();
        std::vector<RelayedEvent> out;
        std::size_t past = bytes.size() + 5;
        CHECK(!Network::readEventsBinary(bytes, past, out, error) && past == bytes.size() + 5);
        Message message{"0"};
        std::size_t pastText = 3;
        CHECK(!Network::readEventsText(message, pastText, out, error) && pastText == 3);
    }
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.count = 65535; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.count = 65; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.sequence = 0; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.type = ""; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.type = std::string(65, 'T'); })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.type = "Bad Type"; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.origin = 0; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.timestamp = -1; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.argCount = 17; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.argCount = 2; })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.argBytes = binaryArg("n", 9, "\x01"); })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) { e.argBytes = binaryArg("n", 0, "\x02"); })));
    for (const double bad : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::denorm_min()}) {
        CHECK(binaryRefused<RelayedEvent>(with([bad](BinaryEvent& e) { e.argBytes = binaryArg("n", 2, f64Bytes(bad)); })));
    }
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) {
        std::string text;
        Network::binary::putString(text, std::string(Net::maxEventTextLength + 1, 'x'));
        e.argBytes = binaryArg("n", 3, text);
    })));
    CHECK(binaryRefused<RelayedEvent>(with([](BinaryEvent& e) {
        e.argCount = 2;
        e.argBytes = binaryArg("n", 0, std::string(1, '\x01')) + binaryArg("n", 0, std::string(1, '\x00'));
    })));
}

// Every shorter prefix of a valid list is refused cleanly.
void truncation()
{
    std::mt19937 rng(42);
    std::vector<RelayedEvent> events;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        Event event = randomEvent(rng);
        event.origin = 5;
        events.push_back({i, event});
    }

    Message message;
    Network::appendEventsText(message, events);
    for (std::size_t length = 0; length < message.size(); ++length) {
        CHECK(textRefused<RelayedEvent>(Message(message.begin(), message.begin() + static_cast<std::ptrdiff_t>(length))));
    }

    std::string bytes;
    Network::appendEventsBinary(bytes, events);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        if (!binaryRefused<RelayedEvent>(bytes.substr(0, length))) {
            CHECK(!"a truncated binary list was accepted");
            return;
        }
    }
}

// Corrupted input never crashes, and whatever does decode is valid: it
// re-encodes without complaint.
template <class Entry>
bool reencodes(const std::vector<Entry>& events)
{
    try {
        Message message;
        std::string bytes;
        Network::appendEventsText(message, events);
        Network::appendEventsBinary(bytes, events);
    } catch (const std::invalid_argument&) {
        return false;
    }
    return true;
}

void corruption()
{
    std::mt19937 rng(7);
    std::vector<RelayedEvent> events;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        Event event = randomEvent(rng);
        event.origin = 9;
        events.push_back({i * 2, event});
    }
    std::string bytes;
    Network::appendEventsBinary(bytes, events);
    Message message;
    Network::appendEventsText(message, events);
    const std::vector<std::string> replacements{"", "0", "1", "-1", "x", "d", "99999999999999999999", "1e999"};

    int accepted = 0;
    for (int i = 0; i < 10000; ++i) {
        std::string mutatedBytes = bytes;
        mutatedBytes[std::uniform_int_distribution<std::size_t>(0, bytes.size() - 1)(rng)] =
            static_cast<char>(std::uniform_int_distribution<int>(0, 255)(rng));
        std::vector<RelayedEvent> decoded;
        std::size_t cursor = 0;
        std::string error;
        if (Network::readEventsBinary(mutatedBytes, cursor, decoded, error)) {
            ++accepted;
            CHECK(reencodes(decoded));
        }

        Message mutatedText = message;
        mutatedText[std::uniform_int_distribution<std::size_t>(0, message.size() - 1)(rng)] =
            replacements[std::uniform_int_distribution<std::size_t>(0, replacements.size() - 1)(rng)];
        cursor = 0;
        if (Network::readEventsText(mutatedText, cursor, decoded, error)) {
            ++accepted;
            CHECK(reencodes(decoded));
        }
    }
    // Some mutations (a different string byte, say) are still valid lists.
    CHECK(accepted > 0);
}
} // namespace

int main()
{
    decimalText();
    valuesAndFields();
    subnormalsTravelAsZero();
    lists();
    locale();
    decimalsAcrossTheRange();
    randomRoundTrips();
    rejectsInvalidEvents();
    rejectsInvalidLists();
    decoderRejectsHandWrittenText();
    decoderRejectsHandWrittenBinary();
    truncation();
    corruption();
    if (failures == 0) {
        std::cout << "Event codec tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
