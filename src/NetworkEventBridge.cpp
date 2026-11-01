#include "NetworkEventBridge.hpp"

#include "EventCodec.hpp"

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
    for (const std::string& type : sharedTypes_) {
        std::string error;
        if (!Network::validateEvent(Event(type), error)) {
            throw std::invalid_argument("cannot share event type '" + type + "': " + error);
        }
    }

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
}

void NetworkEventBridge::apply()
{
    for (Event& event : session_.takeRemoteEvents()) {
        if (event.origin == 0 || sharedTypes_.count(event.type) == 0) {
            continue;
        }
        event.set("sentAt", event.timestamp);
        events_.raise(std::move(event));
    }
}
