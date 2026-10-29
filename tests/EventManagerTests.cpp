#include "Components.hpp"
#include "EventManager.hpp"
#include "GameObject.hpp"
#include "TimeSource.hpp"
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
    manager.subscribe("A", [&](const Event& e) {
        seen.push_back("A");
        CHECK(e.age == 0);
        manager.unsubscribe(removed);
        manager.subscribe("A", [&](const Event&) { seen.push_back("new A"); });
        manager.raise(Event{"B"});
    });
    removed = manager.subscribe("A", [&](const Event&) { seen.push_back("removed A"); });
    manager.subscribe("B", [&](const Event& e) {
        seen.push_back("B");
        CHECK(e.age == 1);
        manager.raise(Event{"C"});
    });
    manager.subscribe("C", [&](const Event& e) {
        seen.push_back("C");
        CHECK(e.age == 2);
    });
    manager.raise(Event{"A"});
    CHECK(manager.dispatch() == 3);
    CHECK((seen == std::vector<std::string>{"A", "B", "C"}));
    manager.raise(Event{"A"});
    CHECK(manager.dispatch() == 3);
    CHECK(seen[4] == "new A");

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
    for (int i = 0; i < 5; ++i) { budget.raise(Event{"burst"}); }
    CHECK(budget.dispatch() == 2);
    CHECK(budget.pending() == 3);
    CHECK(budget.deferredByLimits() == 1);

    EventManager errors(clock);
    int continued = 0;
    errors.subscribe("error", [](const Event&) { throw std::runtime_error("handler"); });
    errors.subscribe("error", [&](const Event&) { ++continued; });
    errors.raise(Event{"error"});
    bool threw = false;
    try { errors.dispatch(); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && continued == 1 && errors.pending() == 0);

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
    anchor.advance(50);
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

int main()
{
    representationAndOrder();
    chainsAndChanges();
    timelineAndThreads();
    listenerLifetime();
    return failures == 0 ? 0 : 1;
}
