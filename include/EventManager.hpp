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
    // Owning thread too (asserted in debug builds), but never throws, so a
    // destructor may call it.
    void unsubscribe(SubscriptionId id);

    // Stamp and queue an event. Delay is in the clock's tics and must be >= 0.
    // With maxPending events already queued, an event less urgent than
    // EventPriority::High is dropped and counted by droppedByLimit().
    void raise(Event event, std::int64_t delayTics = 0);

    // Process due events in bounded rounds. Everything taken in one round runs
    // by priority, then delivery time, then raise order. The budget takes the
    // oldest due events first, so a backlog can delay even a High event. A
    // handler may raise another event, which is eligible in the next round of
    // this call. Returns events handled. A handler's exception is rethrown
    // after the rest have run, except std::out_of_range and
    // std::bad_variant_access from a remote event (origin != 0), which only
    // count in remoteHandlerErrors().
    std::size_t dispatch();
    // Discards every queued event, due or not.
    void clearPending();
    void setLimits(std::size_t maxRounds, std::size_t maxEventsPerDispatch);
    // Must be positive. Applies to every queued event, due or delayed.
    void setMaxPending(std::size_t maxPending);
    std::uint64_t droppedByLimit() const;
    std::uint64_t remoteHandlerErrors() const;
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
    void countRemoteHandlerError(const Event& event);

    const TimeSource& clock_;
    const std::thread::id owner_;
    mutable std::mutex queueMutex_;
    std::priority_queue<Event, std::vector<Event>, LaterFirst> queue_;
    std::uint64_t nextSequence_ = 1;
    std::uint64_t deferredByLimits_ = 0;
    std::uint64_t droppedByLimit_ = 0;
    std::uint64_t remoteHandlerErrors_ = 0;
    // At the 1,000 default budget, a High event waits at most about ten
    // dispatches behind a full queue.
    std::size_t maxPending_ = 10000;
    std::size_t maxRounds_ = 8;
    std::size_t maxEventsPerDispatch_ = 1000;
    bool dispatching_ = false;

    SubscriptionId nextSubscription_ = 1;
    std::map<std::string, std::vector<std::shared_ptr<Subscription>>> byType_;
    std::vector<std::shared_ptr<Subscription>> all_;
    std::map<SubscriptionId, std::shared_ptr<Subscription>> subscriptions_;
};
