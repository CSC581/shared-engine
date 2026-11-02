#pragma once

#include "Event.hpp"
#include "NetworkProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Event lists on the wire, in both client-server formats: the multipart text
// format (one string per field) and the one-frame binary format.
//
// OutgoingEvent and RelayedEvent are in NetworkProtocol.hpp. Client-to-server
// events carry no origin and decode with origin 0, while server-to-client
// events carry the origin the server assigned.
//
// These functions place a list at a position inside a larger message instead
// of owning a whole message, so the protocol can carry events alongside the
// fields it already has. The rules are the same in both formats (see
// validateEvent); limits are in WireFormat.hpp.
namespace Network {

// True when `event` can be encoded: a type and argument names of 1 to
// maxEventNameLength characters from A-Z a-z 0-9 . _ -, at most maxEventArgs
// arguments, text values of at most maxEventTextLength bytes, finite decimals,
// and a timestamp of 0 or more. Otherwise sets `error` to the rule that failed.
// Subnormal decimals (below about 2.2e-308) are valid and travel as zero of
// the same sign, so both formats always carry the same value.
bool validateEvent(const Event& event, std::string& error);

// True when a client-to-server list can be encoded: at most
// maxEventsPerMessage events, each valid, with client sequences that start at
// 1 or more and go up by exactly 1. The same check the encoders and decoders
// make, for a server handed a Request that no decoder saw.
bool validateEvents(const std::vector<OutgoingEvent>& events, std::string& error);

// Append a list to `out`. Throw std::invalid_argument, leaving `out`
// unchanged, if the list breaks a rule: an invalid event, more than
// maxEventsPerMessage events, or sequences out of order.
void appendEventsText(Message& out, const std::vector<OutgoingEvent>& events);
void appendEventsText(Message& out, const std::vector<RelayedEvent>& events);
void appendEventsBinary(std::string& out, const std::vector<OutgoingEvent>& events);
void appendEventsBinary(std::string& out, const std::vector<RelayedEvent>& events);

// Read a list starting at field (text) or byte (binary) `cursor`. On success,
// replace `out` and leave `cursor` just past the list; whatever follows is the
// caller's to check. On failure, set `error` and change neither.
bool readEventsText(const Message& in, std::size_t& cursor,
                    std::vector<OutgoingEvent>& out, std::string& error);
bool readEventsText(const Message& in, std::size_t& cursor,
                    std::vector<RelayedEvent>& out, std::string& error);
bool readEventsBinary(const std::string& in, std::size_t& cursor,
                      std::vector<OutgoingEvent>& out, std::string& error);
bool readEventsBinary(const std::string& in, std::size_t& cursor,
                      std::vector<RelayedEvent>& out, std::string& error);

} // namespace Network
