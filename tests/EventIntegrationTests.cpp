#include "Components.hpp"
#include "EventManager.hpp"
#include "Input.hpp"
#include "TimeSource.hpp"
#include "Timeline.hpp"
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

void chords()
{
    ManualClock clock;
    EventManager events(clock);
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    const std::vector<InputChord> registered{{"dash", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_D}}};
    std::vector<std::string> states;
    events.subscribe(Events::Input, [&](const Event& event) {
        if (event.get<std::string>("kind") == "chord") {
            CHECK(event.get<std::string>("name") == "dash");
            states.push_back(event.get<std::string>("state"));
        }
    });
    Input::setKeyboardStateSource(keys.data(), static_cast<int>(keys.size()));
    const auto frame = [&] {
        Input::update();
        Input::raiseEvents(events, registered);
        events.dispatch();
    };
    frame();
    keys[SDL_SCANCODE_LSHIFT] = true;
    frame();
    CHECK(states.empty());
    keys[SDL_SCANCODE_D] = true;
    frame();
    frame();
    CHECK((states == std::vector<std::string>{"pressed"}));
    keys[SDL_SCANCODE_LSHIFT] = false;
    frame();
    CHECK((states == std::vector<std::string>{"pressed", "released"}));
    Input::setKeyboardStateSource(nullptr, 0);
}

void collisionPhases()
{
    ManualClock clock;
    EventManager events(clock);
    World world;
    GameObject& player = world.create("player");
    player.add<Transform>(0, 0, 10, 10);
    player.add<Collider>();
    player.setActive(true);
    GameObject& zone = world.create("deathzone");
    zone.add<Transform>(5, 0, 10, 10);
    auto& zoneCollider = zone.add<Collider>(Collider::Kind::Trigger);
    int callbacks = 0;
    zoneCollider.onEnter = [&](GameObject&, GameObject&) { ++callbacks; };
    zone.setActive(true);

    world.update(FrameTime{0, 0});
    CHECK(callbacks == 1);
    CHECK(events.pending() == 0);   // no manager attached, no events

    world.setEventManager(&events);
    world.update(FrameTime{0, 0});  // already touching: no enter, stay not opted in
    CHECK(events.pending() == 0);

    std::vector<std::string> phases;
    events.subscribe(Events::Collision, [&](const Event& event) {
        CHECK(event.get<std::int64_t>("a") == player.id());
        CHECK(event.get<std::int64_t>("b") == zone.id());
        CHECK(event.get<std::string>("tagB") == "deathzone");
        CHECK(event.get<bool>("trigger"));
        phases.push_back(event.get<std::string>("phase"));
    });
    zoneCollider.raiseStayEvents = true;
    world.update(FrameTime{0, 0});
    zone.setActive(false);
    world.update(FrameTime{0, 0});
    zone.setActive(true);
    world.update(FrameTime{0, 0});
    events.dispatch();
    CHECK((phases == std::vector<std::string>{"stay", "exit", "enter"}));
}

// Collision raises DeathEvent, which raises a delayed SpawnEvent that moves
// the player back. The delay waits while game time is paused.
void deathRespawnChain()
{
    ManualClock anchor;
    Timeline gameTime(anchor, 1);
    EventManager events(gameTime);
    World world;
    world.setEventManager(&events);
    constexpr std::int64_t respawnDelay = 100;

    GameObject& player = world.create("player");
    auto& transform = player.add<Transform>(0, 0, 10, 10);
    player.add<Collider>();
    player.setActive(true);
    GameObject& zone = world.create("deathzone");
    zone.add<Transform>(5, 0, 10, 10);
    zone.add<Collider>(Collider::Kind::Trigger);
    zone.setActive(true);

    events.subscribe(Events::Collision, [&](const Event& event) {
        if (event.get<std::string>("phase") == "enter" && event.get<std::string>("tagB") == "deathzone") {
            Event death{Events::Death};
            death.set("object", event.get<std::int64_t>("a")).set("cause", "deathzone");
            events.raise(death);
        }
    });
    events.subscribe(Events::Death, [&](const Event& event) {
        Event spawn{Events::Spawn};
        spawn.set("object", event.get<std::int64_t>("object")).set("x", 100.0).set("y", 0.0);
        events.raise(spawn, respawnDelay);
    });
    events.subscribe(Events::Spawn, [&](const Event& event) {
        GameObject* object = world.find(static_cast<ObjectId>(event.get<std::int64_t>("object")));
        if (object != nullptr) {
            object->get<Transform>()->x = static_cast<float>(event.get<double>("x"));
            object->get<Transform>()->y = static_cast<float>(event.get<double>("y"));
        }
    });

    world.update(FrameTime{0, 0});
    CHECK(events.dispatch() == 2);  // collision, then death in a second round
    CHECK(events.pending() == 1);
    gameTime.pause();
    anchor.advance(respawnDelay * 2);
    events.dispatch();
    CHECK(transform.x == 0.0F);
    gameTime.unpause();
    anchor.advance(respawnDelay - 1);
    events.dispatch();
    CHECK(transform.x == 0.0F);
    anchor.advance(1);
    CHECK(events.dispatch() == 1);
    CHECK(transform.x == 100.0F);
}

// The course boilerplate spawned logos from a collision handler while its
// main loop was still walking the logo list. Here the handlers run in
// dispatch(), after the collision pass, and every requested object appears.
void spawnWhileManyCollide()
{
    ManualClock clock;
    EventManager events(clock);
    World world;
    world.setEventManager(&events);
    constexpr int logos = 50;

    GameObject& wall = world.create("wall");
    wall.add<Transform>(0, 0, 1000, 10);
    wall.add<Collider>(Collider::Kind::Trigger);
    wall.setActive(true);
    for (int i = 0; i < logos; ++i) {
        GameObject& logo = world.create("logo");
        logo.add<Transform>(static_cast<float>(i * 20), 0, 10, 10);
        logo.add<Collider>(Collider::Kind::Trigger, 1U);
        logo.setActive(true);
    }
    const std::size_t before = world.objects().size();

    int spawned = 0;
    events.subscribe(Events::Collision, [&](const Event& event) {
        if (event.get<std::string>("phase") == "enter" && event.get<std::string>("tagA") == "wall") {
            Event spawn{Events::Spawn};
            spawn.set("kind", "logo").set("x", 0.0).set("y", 100.0);
            events.raise(spawn);
        }
    });
    events.subscribe(Events::Spawn, [&](const Event& event) {
        GameObject& logo = world.create("spawned");
        logo.add<Transform>(static_cast<float>(event.get<double>("x")),
                            static_cast<float>(event.get<double>("y")), 10, 10);
        logo.setActive(true);
        ++spawned;
    });

    world.update(FrameTime{0, 0});
    CHECK(spawned == 0);
    CHECK(world.objects().size() == before);
    events.dispatch();
    CHECK(spawned == logos);
    CHECK(world.findAllByTag("spawned").size() == static_cast<std::size_t>(logos));
}
}

int main()
{
    inputTransitions();
    chords();
    collisionPhases();
    deathRespawnChain();
    spawnWhileManyCollide();
    return failures == 0 ? 0 : 1;
}
