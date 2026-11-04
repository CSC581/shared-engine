#include "EventManager.hpp"

#include <algorithm>
#include <cassert>
#include <exception>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

EventManager::EventManager(const TimeSource& clock)
    : clock_(clock), owner_(std::this_thread::get_id())
{
}

bool EventManager::LaterFirst::operator()(const Event& a, const Event& b) const
{
    return std::tie(a.deliverAt, a.priority, a.sequence) >
           std::tie(b.deliverAt, b.priority, b.sequence);
}

void EventManager::requireOwner() const
{
    if (std::this_thread::get_id() != owner_) {
        throw std::logic_error("EventManager registration and dispatch require the owning thread");
    }
}

EventManager::SubscriptionId EventManager::subscribe(const std::string& type, Handler handler)
{
    requireOwner();
    if (type.empty() || !handler) {
        throw std::invalid_argument("EventManager subscription needs a type and handler");
    }
    auto subscription = std::make_shared<Subscription>(Subscription{nextSubscription_++, std::move(handler)});
    byType_[type].push_back(subscription);
    subscriptions_[subscription->id] = subscription;
    return subscription->id;
}

EventManager::SubscriptionId EventManager::subscribeAll(Handler handler)
{
    requireOwner();
    if (!handler) {
        throw std::invalid_argument("EventManager subscription needs a handler");
    }
    auto subscription = std::make_shared<Subscription>(Subscription{nextSubscription_++, std::move(handler)});
    all_.push_back(subscription);
    subscriptions_[subscription->id] = subscription;
    return subscription->id;
}

void EventManager::unsubscribe(SubscriptionId id)
{
    // Owning thread only, like subscribe(). Checked with assert rather than
    // requireOwner(): EventListener calls this from its destructor, where a
    // throw would end the program. Debug builds still catch a wrong thread.
    assert(std::this_thread::get_id() == owner_ && "EventManager::unsubscribe requires the owning thread");
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end()) {
        return;
    }
    it->second->active = false;
    subscriptions_.erase(it);
    for (auto& entry : byType_) {
        auto& list = entry.second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [id](const auto& item) { return item->id == id; }), list.end());
    }
    all_.erase(std::remove_if(all_.begin(), all_.end(),
                              [id](const auto& item) { return item->id == id; }), all_.end());
}

void EventManager::raise(Event event, std::int64_t delayTics)
{
    if (event.type.empty() || delayTics < 0) {
        throw std::invalid_argument("EventManager needs a type and nonnegative delay");
    }
    const std::int64_t now = clock_.now();
    if (now > std::numeric_limits<std::int64_t>::max() - delayTics) {
        throw std::overflow_error("Event delivery time overflow");
    }
    event.timestamp = now;
    event.deliverAt = now + delayTics;
    const std::lock_guard<std::mutex> lock(queueMutex_);
    // High is exempt, so an urgent event is never lost to a runaway chain.
    if (queue_.size() >= maxPending_ && event.priority > EventPriority::High) {
        ++droppedByLimit_;
        return;
    }
    event.sequence = nextSequence_++;
    queue_.push(std::move(event));
}

// Called from inside a catch block: a local event's error goes on to the caller.
void EventManager::countRemoteHandlerError(const Event& event)
{
    if (event.origin == 0) {
        throw;
    }
    const std::lock_guard<std::mutex> lock(queueMutex_);
    ++remoteHandlerErrors_;
}

void EventManager::deliver(const Event& event, std::exception_ptr& firstError)
{
    std::vector<std::shared_ptr<Subscription>> listeners;
    const auto it = byType_.find(event.type);
    if (it != byType_.end()) {
        listeners = it->second;
    }
    listeners.insert(listeners.end(), all_.begin(), all_.end());

    for (const auto& listener : listeners) {
        if (!listener->active) {
            continue;
        }
        try {
            // Another player chooses a remote event's arguments, so a missing
            // or wrongly typed one must not stop this game.
            try {
                listener->handler(event);
            } catch (const std::out_of_range&) {
                countRemoteHandlerError(event);
            } catch (const std::bad_variant_access&) {
                countRemoteHandlerError(event);
            }
        } catch (...) {
            if (!firstError) {
                firstError = std::current_exception();
            }
        }
    }
}

std::size_t EventManager::dispatch()
{
    requireOwner();
    if (dispatching_) {
        throw std::logic_error("EventManager::dispatch cannot be called from a handler");
    }
    dispatching_ = true;
    struct ResetDispatch {
        bool& flag;
        ~ResetDispatch() { flag = false; }
    } reset{dispatching_};
    std::size_t handled = 0;
    std::exception_ptr firstError;
    for (std::size_t round = 0; round < maxRounds_ && handled < maxEventsPerDispatch_; ++round) {
        std::vector<Event> ready;
        const std::int64_t now = clock_.now();
        {
            const std::lock_guard<std::mutex> lock(queueMutex_);
            while (!queue_.empty() && queue_.top().deliverAt <= now &&
                   ready.size() < maxEventsPerDispatch_ - handled) {
                ready.push_back(queue_.top());
                queue_.pop();
            }
        }
        if (ready.empty()) {
            break;
        }
        // Raises in one frame get different microsecond stamps, so priority
        // only means something across everything due at this point.
        std::stable_sort(ready.begin(), ready.end(),
                         [](const Event& a, const Event& b) { return a.priority < b.priority; });
        for (const Event& event : ready) {
            deliver(event, firstError);
            ++handled;
        }
    }
    {
        const std::lock_guard<std::mutex> lock(queueMutex_);
        if (!queue_.empty() && queue_.top().deliverAt <= clock_.now()) {
            ++deferredByLimits_;
        }
    }
    if (firstError) {
        std::rethrow_exception(firstError);
    }
    return handled;
}

void EventManager::clearPending()
{
    const std::lock_guard<std::mutex> lock(queueMutex_);
    queue_ = {};
}

void EventManager::setLimits(std::size_t maxRounds, std::size_t maxEventsPerDispatch)
{
    requireOwner();
    if (maxRounds == 0 || maxEventsPerDispatch == 0) {
        throw std::invalid_argument("EventManager limits must be positive");
    }
    maxRounds_ = maxRounds;
    maxEventsPerDispatch_ = maxEventsPerDispatch;
}

void EventManager::setMaxPending(std::size_t maxPending)
{
    if (maxPending == 0) {
        throw std::invalid_argument("EventManager pending limit must be positive");
    }
    const std::lock_guard<std::mutex> lock(queueMutex_);
    maxPending_ = maxPending;
}

std::uint64_t EventManager::droppedByLimit() const
{
    const std::lock_guard<std::mutex> lock(queueMutex_);
    return droppedByLimit_;
}

std::uint64_t EventManager::remoteHandlerErrors() const
{
    const std::lock_guard<std::mutex> lock(queueMutex_);
    return remoteHandlerErrors_;
}

std::size_t EventManager::pending() const
{
    const std::lock_guard<std::mutex> lock(queueMutex_);
    return queue_.size();
}

std::uint64_t EventManager::deferredByLimits() const
{
    const std::lock_guard<std::mutex> lock(queueMutex_);
    return deferredByLimits_;
}
