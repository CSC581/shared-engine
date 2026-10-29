#include "Components.hpp"
#include "EventManager.hpp"
#include "Input.hpp"
#include "TimeSource.hpp"
#include "World.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
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

void inputTransitions()
{
    ManualClock clock;
    EventManager events(clock);
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    std::vector<std::string> states;
    events.subscribe(Events::Input, [&](const Event& event) {
        CHECK(event.get<std::int64_t>("key") == SDL_SCANCODE_SPACE);
        CHECK(event.get<std::string>("kind") == "key");
        states.push_back(event.get<std::string>("state"));
    });
    Input::setKeyboardStateSource(keys.data(), static_cast<int>(keys.size()));
    Input::update();
    Input::raiseEvents(events);
    CHECK(events.dispatch() == 0);
    keys[SDL_SCANCODE_SPACE] = true;
    Input::update();
    Input::raiseEvents(events);
    CHECK(events.dispatch() == 1);
    Input::update();
    Input::raiseEvents(events);
    CHECK(events.dispatch() == 0);
    keys[SDL_SCANCODE_SPACE] = false;
    Input::update();
    Input::raiseEvents(events);
    CHECK(events.dispatch() == 1);
    CHECK((states == std::vector<std::string>{"pressed", "released"}));
    Input::setKeyboardStateSource(nullptr, 0);
}

void collisionDeathSpawn()
{
    ManualClock clock;
    EventManager events(clock);
    World world;
    world.setEventManager(&events);
    int callbackCount = 0;
    int deaths = 0;
    int spawns = 0;

    GameObject& player = world.create("player");
    player.add<Transform>(0, 0, 10, 10);
    player.add<Collider>();
    player.setActive(true);
    GameObject& zone = world.create("deathzone");
    zone.add<Transform>(5, 0, 10, 10);
    auto& collider = zone.add<Collider>(Collider::Kind::Trigger);
    collider.onEnter = [&](GameObject&, GameObject&) { ++callbackCount; };
    zone.setActive(true);

    events.subscribe(Events::Collision, [&](const Event& event) {
        CHECK(event.get<std::int64_t>("a") == player.id());
        CHECK(event.get<std::int64_t>("b") == zone.id());
        CHECK(event.get<bool>("trigger"));
        if (event.get<std::string>("phase") == "enter") {
            Event death{Events::Death};
            death.set("object", player.id()).set("cause", "deathzone");
            events.raise(death);
        }
    });
    events.subscribe(Events::Death, [&](const Event& event) {
        ++deaths;
        CHECK(event.get<std::string>("cause") == "deathzone");
        Event spawn{Events::Spawn};
        spawn.set("kind", "replacement");
        events.raise(spawn);
    });
    events.subscribe(Events::Spawn, [&](const Event& event) {
        ++spawns;
        CHECK(event.get<std::string>("kind") == "replacement");
        GameObject& replacement = world.create("replacement");
        replacement.add<Transform>(20, 0, 10, 10);
        replacement.setActive(true);
    });

    world.update(FrameTime{0, 0});
    CHECK(callbackCount == 1);
    CHECK(world.findByTag("replacement") == nullptr);
    CHECK(events.dispatch() == 3);
    CHECK(deaths == 1 && spawns == 1);
    CHECK(world.findByTag("replacement") != nullptr);
    world.update(FrameTime{0, 0});
    CHECK(events.dispatch() == 0);
    zone.setActive(false);
    world.update(FrameTime{0, 0});
    CHECK(events.dispatch() == 1);
}
}

int main()
{
    inputTransitions();
    collisionDeathSpawn();
    return failures == 0 ? 0 : 1;
}
