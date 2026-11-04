#include "NetworkEventBridge.hpp"

#include "EventCodec.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

NetworkEventBridge::NetworkEventBridge(EventManager& events, Multiplayer::Session& session,
                                       std::vector<std::string> sharedTypes)
    : events_(events),
      session_(session),
      sharedTypes_(sharedTypes.begin(), sharedTypes.end())
{
    if (session_.mode() != Multiplayer::Mode::ClientServer) {
        throw std::logic_error("events require client-server mode");
    }
    if (!session_.supportsEvents()) {
        throw std::logic_error("events require the text format");
    }
    // Two bridges would split takeRemoteEvents() and publish each event twice.
    if (session_.hasBridge_) {
        throw std::logic_error("a session can have only one event bridge");
    }
    for (const std::string& type : sharedTypes_) {
        std::string error;
        if (!Network::validateEvent(Event(type), error)) {
            throw std::invalid_argument("cannot share event type '" + type + "': " + error);
        }
    }
    session_.hasBridge_ = true;

    subscriptions_.reserve(sharedTypes_.size());
    for (const std::string& type : sharedTypes_) {
        subscriptions_.push_back(events_.subscribe(type, [this](const Event& event) {
            if (event.origin == 0) {
                session_.publishEvent(event);
            }
        }));
    }
}

NetworkEventBridge::~NetworkEventBridge()
{
    for (EventManager::SubscriptionId id : subscriptions_) {
        events_.unsubscribe(id);
    }
    session_.hasBridge_ = false;
}

void NetworkEventBridge::apply()
{
    for (Event& event : session_.takeRemoteEvents()) {
        if (event.origin == 0 || sharedTypes_.count(event.type) == 0) {
            continue;
        }
        // The sender's object IDs mean nothing here, or name the wrong object.
        if (event.type == Events::Death || event.type == Events::Spawn || event.type == Events::Collision) {
            event.args.erase("object");
            event.args.erase("a");
            event.args.erase("b");
        }
        event.priority = std::clamp(event.priority, EventPriority::High, EventPriority::Low);
        event.set("net.sentAt", event.timestamp);
        events_.raise(std::move(event));
    }
}
