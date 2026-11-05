#include "Components.hpp"
#include "EventManager.hpp"
#include "GameObject.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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

void representationAndOrder()
{
    ManualClock clock;
    EventManager manager(clock);
    std::vector<std::string> seen;
    manager.subscribeAll([&](const Event& e) { seen.push_back(e.type); });

    Event data{"Data"};
    data.set("flag", true).set("count", 12).set("value", 1.25).set("text", "pressed");
    CHECK(data.get<bool>("flag"));
    CHECK(data.get<std::int64_t>("count") == 12);
    CHECK(data.get<double>("value") == 1.25);
    CHECK(data.get<std::string>("text") == "pressed");
    CHECK(!data.has("missing"));
    CHECK(data.getOr<std::int64_t>("missing", 4) == 4);

    manager.raise(Event{"later"}, 5);
    manager.raise(Event{"normal"});
    Event high{"high"};
    high.priority = EventPriority::High;
    manager.raise(high);
    manager.raise(Event{"normal2"});
    CHECK(manager.dispatch() == 3);
    CHECK((seen == std::vector<std::string>{"high", "normal", "normal2"}));
    clock.advance(5);
    CHECK(manager.dispatch() == 1);
    CHECK(seen.back() == "later");

    EventManager selective(clock);
    std::vector<std::string> order;
    selective.subscribe("one", [&](const Event&) { order.push_back("exact"); });
    const auto allId = selective.subscribeAll([&](const Event&) { order.push_back("all"); });
    selective.raise(Event{"one"});
    selective.raise(Event{"two"});
    CHECK(selective.dispatch() == 2);
    CHECK((order == std::vector<std::string>{"exact", "all", "all"}));
    selective.unsubscribe(allId);
    selective.raise(Event{"two"});
    CHECK(selective.dispatch() == 1);
    CHECK(order.size() == 3);
}

void chainsAndChanges()
{
    ManualClock clock;
    EventManager manager(clock);
    std::vector<std::string> seen;
    EventManager::SubscriptionId removed = 0;
    manager.subscribe("A", [&](const Event&) {
        seen.push_back("A");
        manager.unsubscribe(removed);
        manager.subscribe("A", [&](const Event&) { seen.push_back("new A"); });
        manager.raise(Event{"B"});
    });
    removed = manager.subscribe("A", [&](const Event&) { seen.push_back("removed A"); });
    manager.subscribe("B", [&](const Event&) {
        seen.push_back("B");
        manager.raise(Event{"C"});
    });
    manager.subscribe("C", [&](const Event&) { seen.push_back("C"); });
    manager.raise(Event{"A"});
    CHECK(manager.dispatch() == 3);
    CHECK((seen == std::vector<std::string>{"A", "B", "C"}));
    manager.raise(Event{"A"});
    CHECK(manager.dispatch() == 3);
    CHECK(seen[4] == "new A");

    // A high-priority event raised by a handler waits for the next round.
    EventManager rounds(clock);
    std::vector<std::string> order;
    rounds.subscribe("first", [&](const Event&) {
        order.push_back("first");
        Event urgent{"urgent"};
        urgent.priority = EventPriority::High;
        rounds.raise(urgent);
    });
    rounds.subscribe("second", [&](const Event&) { order.push_back("second"); });
    rounds.subscribe("urgent", [&](const Event&) { order.push_back("urgent"); });
    rounds.raise(Event{"first"});
    rounds.raise(Event{"second"});
    CHECK(rounds.dispatch() == 3);
    CHECK((order == std::vector<std::string>{"first", "second", "urgent"}));

    EventManager limited(clock);
    limited.setLimits(2, 3);
    int handled = 0;
    limited.subscribe("loop", [&](const Event&) {
        ++handled;
        limited.raise(Event{"loop"});
    });
    limited.raise(Event{"loop"});
    CHECK(limited.dispatch() == 2);
    CHECK(limited.pending() == 1);
    CHECK(limited.deferredByLimits() == 1);
    CHECK(limited.dispatch() == 2);
    CHECK(handled == 4);

    EventManager budget(clock);
    budget.setLimits(8, 2);
    std::vector<std::uint64_t> sequences;
    budget.subscribe("burst", [&](const Event& e) { sequences.push_back(e.sequence); });
    for (int i = 0; i < 5; ++i) { budget.raise(Event{"burst"}); }
    CHECK(budget.dispatch() == 2);
    CHECK(budget.pending() == 3);
    CHECK(budget.deferredByLimits() == 1);
    CHECK(budget.dispatch() == 2);
    CHECK(budget.dispatch() == 1);
    CHECK((sequences == std::vector<std::uint64_t>{1, 2, 3, 4, 5}));

    EventManager errors(clock);
    int continued = 0;
    errors.subscribe("error", [](const Event&) { throw std::runtime_error("handler"); });
    errors.subscribe("error", [&](const Event&) { ++continued; });
    errors.raise(Event{"error"});
    bool threw = false;
    try { errors.dispatch(); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && continued == 1 && errors.pending() == 0);
    errors.subscribe("ok", [&](const Event&) { ++continued; });
    errors.raise(Event{"ok"});
    CHECK(errors.dispatch() == 1 && continued == 2);

    EventManager reentrant(clock);
    bool rejected = false;
    reentrant.subscribe("outer", [&](const Event&) {
        try { reentrant.dispatch(); } catch (const std::logic_error&) { rejected = true; }
    });
    reentrant.raise(Event{"outer"});
    CHECK(reentrant.dispatch() == 1);
    CHECK(rejected);
}

void timelineAndThreads()
{
    ManualClock anchor;
    Timeline timeline(anchor, 10);
    EventManager manager(timeline);
    int count = 0;
    manager.subscribe("tick", [&](const Event&) { ++count; });
    manager.raise(Event{"tick"}, 10);
    timeline.pause();
    anchor.advance(100);
    CHECK(manager.dispatch() == 0);
    manager.raise(Event{"tick"});
    CHECK(manager.dispatch() == 1);
    timeline.unpause();
    timeline.setScale(2.0);
    anchor.advance(40);   // 8 tics at 2x (4 at 1x): not due yet
    CHECK(manager.dispatch() == 0);
    anchor.advance(10);   // 10 tics after 50 anchor units, half the 1x time
    CHECK(manager.dispatch() == 1);

    constexpr int workers = 4;
    constexpr int perWorker = 40;
    std::vector<std::thread> threads;
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < perWorker; ++j) {
                manager.raise(Event{"tick"});
            }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    CHECK(manager.dispatch() == workers * perWorker);
    CHECK(count == 2 + workers * perWorker);
}

// On a running clock, events raised in one frame get different stamps, and
// priority must still decide among everything due.
void priorityWorksOnARunningClock()
{
    ManualClock clock;
    EventManager manager(clock);
    std::vector<std::string> seen;
    manager.subscribeAll([&](const Event& event) { seen.push_back(event.type); });

    Event low{"low"};
    low.priority = EventPriority::Low;
    Event high{"high"};
    high.priority = EventPriority::High;
    manager.raise(low);
    clock.advance(1);
    manager.raise(Event{"normal"});
    clock.advance(1);
    manager.raise(high);
    clock.advance(1);
    manager.raise(Event{"normal2"});
    CHECK(manager.dispatch() == 4);
    CHECK((seen == std::vector<std::string>{"high", "normal", "normal2", "low"}));

    // An event not yet due still waits, whatever its priority.
    seen.clear();
    manager.raise(high, 5);
    manager.raise(low);
    CHECK(manager.dispatch() == 1);
    CHECK((seen == std::vector<std::string>{"low"}));
    clock.advance(5);
    CHECK(manager.dispatch() == 1);
    CHECK(seen.back() == "high");

    // The real clock, as in Engine: stamps differ, the order does not.
    RealTimeClock real;
    Timeline game(real, kNsPerUs);
    EventManager running(game);
    bool lowFirst = false;
    std::string first;
    running.subscribeAll([&](const Event& event) {
        if (first.empty()) {
            first = event.type;
        }
    });
    for (int i = 0; i < 2000; ++i) {
        first.clear();
        running.raise(low);
        std::this_thread::yield();
        running.raise(high);
        running.dispatch();
        lowFirst |= first == "low";
    }
    CHECK(!lowFirst);
}

// A fan-out chain stays bounded, and an urgent event still gets through.
void aRunawayChainStaysBounded()
{
    ManualClock clock;
    EventManager manager(clock);
    std::uint64_t spreads = 0;
    manager.subscribe("spread", [&](const Event&) {
        ++spreads;
        manager.raise(Event{"spread"});
        manager.raise(Event{"spread"});
    });
    int urgentAt = -1;
    int dispatches = 0;
    manager.subscribe("urgent", [&](const Event&) { urgentAt = dispatches; });

    manager.raise(Event{"spread"});
    constexpr int raisedAt = 100;
    for (dispatches = 0; dispatches < 200; ++dispatches) {
        clock.advance(1);
        if (dispatches == raisedAt) {
            CHECK(manager.pending() >= 10000 - 1000);
            Event urgent{"urgent"};
            urgent.priority = EventPriority::High;
            manager.raise(urgent);
        }
        manager.dispatch();
        CHECK(manager.pending() <= 10001);
    }
    CHECK(manager.droppedByLimit() > 0);
    CHECK(urgentAt >= raisedAt && urgentAt <= raisedAt + 11);

    // Only events less urgent than High are dropped; the limit can be changed.
    ManualClock otherClock;
    EventManager small(otherClock);
    small.setMaxPending(2);
    small.raise(Event{"a"});
    small.raise(Event{"b"});
    small.raise(Event{"dropped"});
    Event kept{"kept"};
    kept.priority = EventPriority::High;
    small.raise(kept);
    CHECK(small.pending() == 3);
    CHECK(small.droppedByLimit() == 1);
    bool threw = false;
    try {
        small.setMaxPending(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Clearing makes room again.
    small.clearPending();
    CHECK(small.pending() == 0);
    small.raise(Event{"after"});
    CHECK(small.pending() == 1);
}

void listenerLifetime()
{
    ManualClock clock;
    EventManager manager(clock);
    World world;
    int calls = 0;
    GameObject& object = world.create("listener");
    auto& listener = object.add<EventListener>(manager);
    listener.listen("notice", [&](const Event&) { ++calls; });
    object.setActive(true);
    manager.raise(Event{"notice"});
    manager.dispatch();
    CHECK(calls == 1);
    object.setActive(false);
    manager.raise(Event{"notice"});
    manager.dispatch();
    CHECK(calls == 1);
    world.destroy(object.id());
    world.flushDestroyed();
    manager.raise(Event{"notice"});
    manager.dispatch();
    CHECK(calls == 1);
}
}

void badRemoteArgumentsDoNotStopTheGame()
{
    ManualClock clock;
    EventManager manager(clock);
    int ran = 0;
    manager.subscribe("Hit", [&](const Event& event) {
        ++ran;
        event.get<std::int64_t>("damage");
    });

    Event remote("Hit");
    remote.origin = 7;
    manager.raise(remote);
    remote.set("damage", "lots");
    manager.raise(remote);
    manager.dispatch();
    CHECK(ran == 2);
    CHECK(manager.remoteHandlerErrors() == 2);

    // The same mistake on a local event is this game's bug and still surfaces.
    manager.raise(Event("Hit"));
    bool threw = false;
    try {
        manager.dispatch();
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(manager.remoteHandlerErrors() == 2);

    // Only bad data is forgiven: a remote event cannot hide other failures.
    manager.subscribe("Boom", [](const Event&) { throw std::runtime_error("bug"); });
    remote.type = "Boom";
    manager.raise(remote);
    threw = false;
    try {
        manager.dispatch();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

int main()
{
    badRemoteArgumentsDoNotStopTheGame();
    representationAndOrder();
    chainsAndChanges();
    timelineAndThreads();
    priorityWorksOnARunningClock();
    aRunawayChainStaysBounded();
    listenerLifetime();
    return failures == 0 ? 0 : 1;
}
