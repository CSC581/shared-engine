#include "Components.hpp"
#include "EventManager.hpp"
#include "FrameWorkers.hpp"
#include "TimeSource.hpp"
#include "World.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
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

template <class Exception, class Action>
void throws(Action action)
{
    bool expected = false;
    try {
        action();
    } catch (const Exception&) {
        expected = true;
    } catch (...) {
    }
    CHECK(expected);
}

struct Counted : Component {
    int& destroyed;
    explicit Counted(int& destroyed) : destroyed(destroyed) {}
    ~Counted() override { ++destroyed; }
};

static_assert(!std::is_copy_constructible_v<World>);
static_assert(!std::is_move_constructible_v<World>);

void heapModeKeepsExistingBehavior()
{
    World world;
    CHECK(world.objectCapacity() == 0);
    auto* first = world.tryCreate("first");
    CHECK(first != nullptr);
    if (first == nullptr) return;
    CHECK(first->tag() == "first" && !first->isActive());
    first->add<Motion>(5.0F, 0.0F);
    CHECK(&first->get<Motion>()->owner() == first);
    auto* address = first;
    const auto id = first->id();
    for (int i = 0; i < 100; ++i) world.create("later");
    CHECK(world.find(id) == address);
    world.destroy(id);
    CHECK(world.find(id) == address);
    world.flushDestroyed();
    CHECK(world.find(id) == nullptr);
    CHECK(world.objects().size() == 100);
}

void invalidWorldCapacity()
{
    throws<std::invalid_argument>([] { World world(0); });
    constexpr auto impossible = std::numeric_limits<std::size_t>::max() / sizeof(GameObject) + 1;
    throws<std::length_error>([] { World world(impossible); });
}

void exhaustionLeavesObjectsUnchanged()
{
    World world(2);
    CHECK(world.objectCapacity() == 2);
    CHECK(world.objects().empty());
    auto& first = world.create();
    auto& second = world.create("second");
    const auto firstId = first.id();
    const auto secondId = second.id();
    CHECK(first.tag().empty());
    CHECK(!first.isActive() && !second.isActive());
    for (int i = 0; i < 3; ++i) {
        CHECK(world.tryCreate("overflow") == nullptr);
        throws<std::bad_alloc>([&] { world.create("overflow"); });
        CHECK(world.objects().size() == 2);
        CHECK(world.objectCapacity() == 2);
        CHECK(world.find(firstId) == &first && world.find(secondId) == &second);
        CHECK(world.objects()[0].get() == &first && world.objects()[1].get() == &second);
        CHECK(first.tag().empty() && second.tag() == "second");
    }
}

void activationDoesNotReleaseSlots()
{
    World world(1);
    auto& object = world.create("inactive");
    CHECK(world.tryCreate() == nullptr);
    object.setActive(true);
    object.setActive(false);
    world.update(FrameTime{0.1, 0});
    CHECK(world.tryCreate() == nullptr);
    CHECK(world.objects().size() == 1);
}

void destructionIsDeferredAndIdempotent()
{
    int destroyed = 0;
    World world(1);
    auto& object = world.create("old");
    object.add<Counted>(destroyed);
    const auto id = object.id();
    world.destroy(id);
    world.destroy(id);
    world.destroy(0);
    CHECK(world.find(id) == &object);
    CHECK(destroyed == 0);
    CHECK(world.tryCreate() == nullptr);
    world.flushDestroyed();
    CHECK(destroyed == 1);
    CHECK(world.find(id) == nullptr);
    CHECK(world.objects().empty());
    world.flushDestroyed();
    world.destroy(id);
    world.flushDestroyed();
    CHECK(destroyed == 1);
    CHECK(world.tryCreate("new") != nullptr);
    CHECK(world.tryCreate("extra") == nullptr);
}

void replacementHasFreshIdentityAndComponents()
{
    World world(1);
    auto& old = world.create("old");
    old.add<Motion>(123.0F, 456.0F);
    old.setActive(true);
    const auto oldId = old.id();
    const auto address = reinterpret_cast<std::uintptr_t>(&old);
    world.destroy(oldId);
    world.flushDestroyed();
    auto& replacement = world.create("new");
    const auto newId = replacement.id();
    CHECK(reinterpret_cast<std::uintptr_t>(&replacement) == address);
    CHECK(newId > oldId);
    CHECK(replacement.tag() == "new" && !replacement.isActive());
    CHECK(replacement.components().empty());
    CHECK(!replacement.has<Motion>() && !replacement.has<Transform>());
    CHECK(world.find(oldId) == nullptr);
    auto& motion = replacement.add<Motion>(7.0F, 0.0F);
    CHECK(&motion.owner() == &replacement);
    CHECK(&replacement.get<Transform>()->owner() == &replacement);
    CHECK(motion.velocityX == 7.0F);
    world.destroy(oldId);
    world.flushDestroyed();
    CHECK(world.find(newId) == &replacement);
    CHECK(world.tryCreate() == nullptr);
}

void stableAddressesAndCreationOrder()
{
    World world(4);
    std::vector<ObjectId> ids;
    std::vector<GameObject*> addresses;
    for (const char* tag : {"a", "b", "c", "d"}) {
        auto& object = world.create(tag);
        ids.push_back(object.id());
        addresses.push_back(&object);
    }
    world.destroy(ids[0]);
    world.destroy(ids[2]);
    world.flushDestroyed();
    auto& e = world.create("e");
    auto& f = world.create("f");
    const World& view = world;
    CHECK(view.find(ids[1]) == addresses[1] && view.find(ids[3]) == addresses[3]);
    CHECK(view.objects().size() == 4);
    const std::vector<ObjectId> expected{ids[1], ids[3], e.id(), f.id()};
    std::vector<ObjectId> actual;
    for (const auto& object : view.objects()) actual.push_back(object->id());
    CHECK(actual == expected);
}

void lookupsOnlySeeCurrentLifetimes()
{
    World world(2);
    auto& old = world.create("shared");
    old.add<Motion>();
    auto& survivor = world.create("shared");
    const auto oldId = old.id();
    world.destroy(oldId);
    world.flushDestroyed();
    auto& replacement = world.create("new");
    replacement.add<Renderable>(Color{1, 2, 3});
    CHECK(world.findByTag("shared") == &survivor);
    CHECK(world.findAllByTag("shared") == std::vector<GameObject*>{&survivor});
    CHECK(world.findAllWith<Motion>().empty());
    CHECK(world.findAllWith<Renderable>() == std::vector<GameObject*>{&replacement});
}

void cachedUpdatesForgetDestroyedComponents()
{
    World world(2);
    std::vector<int> calls;
    auto& old = world.create("old");
    old.add<Behavior>([&](GameObject&, const FrameTime&) { calls.push_back(1); });
    old.setActive(true);
    auto& survivor = world.create("survivor");
    survivor.add<Behavior>([&](GameObject&, const FrameTime&) { calls.push_back(2); });
    survivor.setActive(true);
    world.updateComponents(FrameTime{0.1, 0});
    CHECK((calls == std::vector<int>{1, 2}));
    const auto id = old.id();
    world.destroy(id);
    world.flushDestroyed();
    auto& replacement = world.create("new");
    replacement.add<Motion>(10.0F, 0.0F);
    replacement.add<Behavior>([&](GameObject& self, const FrameTime&) {
        calls.push_back(3);
        self.get<Motion>()->velocityX = 20.0F;
    });
    replacement.setActive(true);
    calls.clear();
    world.updateComponents(FrameTime{1.0, 0});
    CHECK((calls == std::vector<int>{2, 3}));
    CHECK(replacement.get<Transform>()->x == 20.0F);
    calls.clear();
    world.updateComponents(FrameTime{1.0, 0});
    CHECK((calls == std::vector<int>{2, 3}));
    CHECK(replacement.get<Transform>()->x == 40.0F);
}

void teardownDestroysComponentsAndPendingObjects()
{
    int destroyed = 0;
    {
        World world(4);
        auto& first = world.create();
        first.add<Counted>(destroyed);
        world.create().add<Counted>(destroyed);
        world.create().add<Counted>(destroyed);
        world.destroy(first.id());
        world.flushDestroyed();
        CHECK(destroyed == 1);
        auto& replacement = world.create();
        replacement.add<Counted>(destroyed);
        world.destroy(replacement.id());
    }
    CHECK(destroyed == 4);
}

void listenersUnsubscribeOnFlushAndTeardown()
{
    ManualClock clock;
    EventManager events(clock);
    int oldCalls = 0;
    int newCalls = 0;
    {
        World world(1);
        auto& old = world.create();
        old.add<EventListener>(events).listen("probe", [&](const Event&) { ++oldCalls; });
        old.setActive(true);
        events.raise(Event{"probe"});
        events.dispatch();
        CHECK(oldCalls == 1);
        world.destroy(old.id());
        events.raise(Event{"probe"});
        world.flushDestroyed();
        auto& replacement = world.create();
        replacement.add<EventListener>(events).listen("probe", [&](const Event&) { ++newCalls; });
        replacement.setActive(true);
        events.dispatch();
        CHECK(oldCalls == 1 && newCalls == 1);
    }
    events.raise(Event{"probe"});
    events.dispatch();
    CHECK(oldCalls == 1 && newCalls == 1);
}

void collisionContactsUseIdsRatherThanRecycledAddresses()
{
    ManualClock clock;
    EventManager events(clock);
    World world(2);
    world.setEventManager(&events);
    std::vector<std::string> phases;
    std::vector<std::pair<ObjectId, ObjectId>> pairs;
    events.subscribe(Events::Collision, [&](const Event& event) {
        phases.push_back(event.get<std::string>("phase"));
        pairs.emplace_back(static_cast<ObjectId>(event.get<std::int64_t>("a")),
                           static_cast<ObjectId>(event.get<std::int64_t>("b")));
    });
    const auto make = [&world](const char* tag) -> GameObject& {
        auto& object = world.create(tag);
        object.add<Transform>(0.0F, 0.0F, 10.0F, 10.0F);
        object.add<Collider>(Collider::Kind::Trigger).raiseStayEvents = true;
        object.setActive(true);
        return object;
    };
    auto& survivor = make("survivor");
    auto& old = make("old");
    const auto survivorId = survivor.id();
    const auto oldId = old.id();
    const auto address = reinterpret_cast<std::uintptr_t>(&old);
    world.resolveCollisions();
    events.dispatch();
    world.resolveCollisions();
    events.dispatch();
    CHECK((phases == std::vector<std::string>{"enter", "stay"}));
    world.destroy(oldId);
    world.flushDestroyed();
    auto& replacement = make("replacement");
    CHECK(reinterpret_cast<std::uintptr_t>(&replacement) == address);
    phases.clear();
    pairs.clear();
    world.resolveCollisions();
    events.dispatch();
    CHECK((phases == std::vector<std::string>{"enter"}));
    CHECK((pairs == std::vector<std::pair<ObjectId, ObjectId>>{{survivorId, replacement.id()}}));
}

void callbacksQueueDestructionUntilAfterCollisionPass()
{
    World world(2);
    int aCalls = 0;
    int bCalls = 0;
    ObjectId doomed = 0;
    auto& a = world.create("a");
    a.add<Transform>(0.0F, 0.0F, 10.0F, 10.0F);
    a.add<Collider>(Collider::Kind::Trigger).onEnter = [&](GameObject&, GameObject& other) {
        doomed = other.id();
        world.destroy(doomed);
        CHECK(world.find(doomed) == &other);
        CHECK(world.tryCreate() == nullptr);
        ++aCalls;
    };
    a.setActive(true);
    auto& b = world.create("b");
    b.add<Transform>(0.0F, 0.0F, 10.0F, 10.0F);
    b.add<Collider>(Collider::Kind::Trigger).onEnter = [&](GameObject&, GameObject&) { ++bCalls; };
    b.setActive(true);
    world.resolveCollisions();
    CHECK(aCalls == 1 && bCalls == 1);
    CHECK(world.find(doomed) != nullptr);
    world.flushDestroyed();
    CHECK(world.find(doomed) == nullptr);
    CHECK(world.tryCreate("after-collision") != nullptr);
}

void workersOnlyQueueDestruction()
{
    constexpr std::size_t capacity = 32;
    int destroyed = 0;
    World world(capacity);
    for (const char* tag : {"left", "right"}) {
        for (std::size_t i = 0; i < capacity / 2; ++i) {
            auto& object = world.create(tag);
            object.add<Counted>(destroyed);
            object.add<Behavior>([&](GameObject& self, const FrameTime&) { world.destroy(self.id()); });
            object.setActive(true);
        }
    }
    FrameWorkers workers({
        [&](float dt) { world.updateComponents(FrameTime{dt, 0}, "left"); },
        [&](float dt) { world.updateComponents(FrameTime{dt, 0}, "right"); },
    });
    workers.runFrame(0.1F);
    CHECK(world.objects().size() == capacity);
    CHECK(destroyed == 0);
    CHECK(world.tryCreate() == nullptr);
    world.flushDestroyed();
    CHECK(world.objects().empty());
    CHECK(destroyed == static_cast<int>(capacity));
    for (std::size_t i = 0; i < capacity; ++i) CHECK(world.tryCreate() != nullptr);
    CHECK(world.tryCreate() == nullptr);
}
} // namespace

int main()
{
    const std::pair<const char*, void (*)()> cases[] = {
        {"heap compatibility", heapModeKeepsExistingBehavior},
        {"invalid World capacity", invalidWorldCapacity},
        {"exhaustion preserves objects", exhaustionLeavesObjectsUnchanged},
        {"inactive objects occupy slots", activationDoesNotReleaseSlots},
        {"deferred duplicate destruction", destructionIsDeferredAndIdempotent},
        {"fresh identity on address reuse", replacementHasFreshIdentityAndComponents},
        {"stable pointers and creation order", stableAddressesAndCreationOrder},
        {"lookups forget old lifetimes", lookupsOnlySeeCurrentLifetimes},
        {"update cache after reuse", cachedUpdatesForgetDestroyedComponents},
        {"teardown with pending objects", teardownDestroysComponentsAndPendingObjects},
        {"listener flush and teardown", listenersUnsubscribeOnFlushAndTeardown},
        {"collision identity after reuse", collisionContactsUseIdsRatherThanRecycledAddresses},
        {"collision callback destruction", callbacksQueueDestructionUntilAfterCollisionPass},
        {"worker destruction and capacity recovery", workersOnlyQueueDestruction},
    };
    for (const auto& [name, run] : cases) {
        const int before = failures;
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
        std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
    }
    return failures == 0 ? 0 : 1;
}
