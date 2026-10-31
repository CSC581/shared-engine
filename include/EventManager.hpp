#pragma once

#include "Event.hpp"
#include "TimeSource.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Raising is safe from any thread. Registration and dispatch belong to the
// thread that constructed the manager. Handlers run without the queue lock.
class EventManager {
public:
    using Handler = std::function<void(const Event&)>;
    using SubscriptionId = std::uint64_t;

    explicit EventManager(const TimeSource& clock);

    // Exact-type handlers run in subscription order, before subscribeAll.
    // A subscription added during handling begins with the next event.
    SubscriptionId subscribe(const std::string& type, Handler handler);
    SubscriptionId subscribeAll(Handler handler);
    // Owning thread too, but never throws, so a destructor may call it.
    void unsubscribe(SubscriptionId id);

    // Stamp and queue an event. Delay is in the clock's tics and must be >= 0.
    void raise(Event event, std::int64_t delayTics = 0);

    // Process due events in bounded rounds. A handler may raise another event,
    // which is eligible in the next round of this call. Returns events handled.
    std::size_t dispatch();
    void setLimits(std::size_t maxRounds, std::size_t maxEventsPerDispatch);
    std::size_t pending() const;
    std::uint64_t deferredByLimits() const;

private:
    struct Subscription {
        SubscriptionId id;
        Handler handler;
        bool active = true;
    };
    struct LaterFirst {
        bool operator()(const Event& a, const Event& b) const;
    };

    void requireOwner() const;
    void deliver(const Event& event, std::exception_ptr& firstError);

    const TimeSource& clock_;
    const std::thread::id owner_;
    mutable std::mutex queueMutex_;
    std::priority_queue<Event, std::vector<Event>, LaterFirst> queue_;
    std::uint64_t nextSequence_ = 1;
    std::uint64_t deferredByLimits_ = 0;
    std::size_t maxRounds_ = 8;
    std::size_t maxEventsPerDispatch_ = 1000;
    bool dispatching_ = false;

    SubscriptionId nextSubscription_ = 1;
    std::map<std::string, std::vector<std::shared_ptr<Subscription>>> byType_;
    std::vector<std::shared_ptr<Subscription>> all_;
    std::map<SubscriptionId, std::shared_ptr<Subscription>> subscriptions_;
};
