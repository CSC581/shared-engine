#pragma once

#include "EventManager.hpp"
#include "Multiplayer.hpp"

#include <set>
#include <string>
#include <vector>

// Shares chosen event types between a game's EventManager and the other
// players, through a client-server Multiplayer::Session.
//
// Local events of a shared type (origin 0) are published to the session as
// they are handled. In apply(), events from other players are raised on the
// local manager, so the game's own handlers run for them like any other
// event. A remote event is raised with this machine's game time, because the
// sender's clock means nothing here, and keeps the sender's timestamp as the
// argument "sentAt". Its origin is the sender's player id.
//
// Only listed types cross in either direction. A remote event of any other
// type is ignored, so another player cannot raise, say, InputEvent here. A
// remote event is never published again, so events cannot echo around the
// network.
//
// Construct, call apply() and destroy on the thread that owns the manager.
// The bridge must not outlive the manager or the session.
class NetworkEventBridge {
public:
    // Throws std::logic_error for a peer-to-peer or binary session, which
    // carry no events, and std::invalid_argument for a type that is not a valid event
    // name (see validateEvent in EventCodec.hpp).
    NetworkEventBridge(EventManager& events, Multiplayer::Session& session,
                       std::vector<std::string> sharedTypes);
    ~NetworkEventBridge();

    NetworkEventBridge(const NetworkEventBridge&) = delete;
    NetworkEventBridge& operator=(const NetworkEventBridge&) = delete;

    // Raises the events other players sent. Call once a frame, after
    // session.update().
    void apply();

private:
    EventManager& events_;
    Multiplayer::Session& session_;
    std::set<std::string> sharedTypes_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
};
