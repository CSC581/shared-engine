// Pokemon Hunt's level, collisions and server-side Mewtwo, without a window.
#include "EventManager.hpp"
#include "PokemonHuntBoss.hpp"
#include "PokemonHuntEvents.hpp"
#include "PokemonHuntLevel.hpp"
#include "PokemonHuntPrediction.hpp"
#include "TimeSource.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace PokemonHunt;

constexpr float frame = 1.0F / 60.0F;
constexpr float floorTop = groundY - pokemonSize;

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 0.01F;
}

struct StoppedClock final : TimeSource {
    std::int64_t now() const override { return 0; }
};

// A Pokemon as the game builds one, minus the keyboard and the fight.
GameObject& makeFallingBody(World& world, float x, float y, float velocityX = 0.0F, float velocityY = 0.0F)
{
    GameObject& body = world.create("pokemon");
    body.add<Transform>(x, y, pokemonSize, pokemonSize);
    body.add<Motion>(velocityX, velocityY);
    body.add<Gravity>(gravity);
    body.add<Collider>(Collider::Kind::Solid, hurtLayer | zoneLayer | solidLayer);
    body.setActive(true);
    return body;
}

bool levelIsGameObjects()
{
    World world;
    buildLevel(world);

    const std::vector<GameObject*> platforms = world.findAllByTag("platform");
    bool passed = expect(platforms.size() >= 3, "at least three static platforms");
    for (std::size_t i = 0; i < platforms.size(); ++i) {
        const Transform& a = *platforms[i]->get<Transform>();
        const Renderable* look = platforms[i]->get<Renderable>();
        passed &= expect(look != nullptr && !platforms[i]->has<Motion>(), "static platforms are drawn and never move");
        for (std::size_t j = i + 1; j < platforms.size(); ++j) {
            const Transform& b = *platforms[j]->get<Transform>();
            const Renderable& other = *platforms[j]->get<Renderable>();
            const bool sameColor = look->color.red == other.color.red && look->color.green == other.color.green &&
                                   look->color.blue == other.color.blue;
            passed &= expect(!sameColor && !near(a.x, b.x) && !near(a.width, b.width),
                             "each static platform has its own position, size and colour");
        }
    }

    const std::vector<GameObject*> spawns = world.findAllByTag("spawn");
    passed &= expect(spawns.size() == SpeciesCount, "one spawn point per species");
    for (int kind = 0; kind < SpeciesCount; ++kind) {
        const Transform& spawn = spawnPointFor(world, kind);
        passed &= expect(near(spawn.x, spawnX(kind)) && near(spawn.y, spawnY), "spawn point N is species N's");
        passed &= expect(!spawns[static_cast<std::size_t>(kind)]->has<Renderable>(), "spawn points are hidden");
    }

    GameObject& zone = makeDeathZone(world);
    passed &= expect(!zone.has<Renderable>() && zone.get<Collider>()->kind == Collider::Kind::Trigger,
                     "the death zone is a hidden trigger");
    const std::vector<Network::PlatformPath> paths = movingPlatformPaths();
    passed &= expect(paths.size() >= 2 && paths[0].shape != paths[1].shape,
                     "the server moves platforms in two patterns (linear and circular)");
    return passed;
}

// Runs World frames until the body rests on a platform; returns that platform.
GameObject* fall(World& world, GameObject& pokemon)
{
    const Transform& body = *pokemon.get<Transform>();
    for (int i = 0; i < 240 && body.y < windowHeight + 100.0F; ++i) {
        world.update(FrameTime{frame, 0});
        for (const char* tag : {"platform", "moving-platform"}) {
            for (GameObject* platform : world.findAllByTag(tag)) {
                if (pokemon.get<Motion>()->velocityY == 0.0F && standsOn(body, *platform->get<Transform>())) {
                    return platform;
                }
            }
        }
    }
    return nullptr;
}

bool pokemonFallsOntoTheFloor()
{
    World world;
    buildLevel(world);
    GameObject& pokemon = makeFallingBody(world, 450.0F, 100.0F);
    GameObject* standingOn = fall(world, pokemon);
    return expect(standingOn != nullptr && standingOn->tag() == "platform" && near(pokemon.get<Transform>()->y, floorTop),
                  "the collision pass stands a falling Pokemon on a static platform");
}

// The game's chain, as PokemonHunt.cpp wires it: entering the death zone is a
// CollisionEvent, which raises a DeathEvent, which raises a SpawnEvent.
bool pitsLeadToTheDeathZone()
{
    StoppedClock clock;
    EventManager events(clock);
    World world;
    world.setEventManager(&events);
    buildLevel(world);
    makeDeathZone(world);
    const Transform& spawn = spawnPointFor(world, Squirtle);
    const float pitStart = staticPlatforms[0].x + staticPlatforms[0].width;
    GameObject& pokemon = makeFallingBody(world, pitStart + (staticPlatforms[1].x - pitStart - pokemonSize) * 0.5F, 300.0F);

    std::vector<std::string> chain;
    events.subscribe(Events::Collision, [&](const Event& event) {
        Contact contact;
        if (contactOf(event, pokemon.id(), contact) && contact.otherTag == "death-zone" && contact.phase == "enter") {
            chain.push_back("collision");
            events.raise(deathEvent(pokemon.id(), "pit"));
        }
    });
    events.subscribe(Events::Death, [&](const Event& event) {
        if (isLocalAbout(event, pokemon.id())) {
            chain.push_back("death");
            events.setMaxPending(1);
            events.raise(Event{"Backlog"}); // A full queue must not lose the respawn.
            events.raise(respawnEvent(pokemon.id(), spawn.x, spawn.y));
        }
    });
    events.subscribe(Events::Spawn, [&](const Event& event) {
        chain.push_back("spawn");
        Transform& body = *pokemon.get<Transform>();
        body.x = static_cast<float>(event.get<double>("x"));
        body.y = static_cast<float>(event.get<double>("y"));
    });

    for (int i = 0; i < 240 && chain.empty(); ++i) {
        world.update(FrameTime{frame, 0});
        events.dispatch();
    }
    const Transform& body = *pokemon.get<Transform>();
    return expect(chain == std::vector<std::string>{"collision", "death", "spawn"} && near(body.x, spawn.x) &&
                      near(body.y, spawn.y),
                  "a pit fall is a CollisionEvent, then a DeathEvent, then a SpawnEvent back at the spawn point");
}

bool movingPlatformsAreSolid()
{
    World world;
    buildLevel(world);
    GameObject& platform = makeMovingPlatform(world);
    Transform& box = *platform.get<Transform>();
    box.x = 800.0F;
    box.y = 360.0F;
    box.width = 140.0F;
    box.height = 30.0F;
    platform.setActive(true);

    GameObject& faller = makeFallingBody(world, box.x + 20.0F, 250.0F);
    bool passed = expect(fall(world, faller) == &platform, "a falling Pokemon lands on a moving platform");

    GameObject& jumper = makeFallingBody(world, box.x + 80.0F, box.y + box.height - 2.0F, 0.0F, -300.0F);
    world.update(FrameTime{frame, 0});
    passed &= expect(near(jumper.get<Transform>()->y, box.y + box.height) && jumper.get<Motion>()->velocityY == 0.0F,
                     "a Pokemon jumping into a moving platform from below bumps its head");
    return passed;
}

bool staticPlatformsAreWallsAndTheLevelHasEnds()
{
    World world;
    buildLevel(world);
    const StaticPlatform& second = staticPlatforms[1];

    GameObject& inPit = makeFallingBody(world, second.x - 10.0F, groundY + 20.0F, walkSpeed);
    world.update(FrameTime{frame, 0});
    bool passed = expect(near(inPit.get<Transform>()->x, second.x - pokemonSize),
                         "a Pokemon in a pit cannot walk into the floor");

    GameObject& atStart = makeFallingBody(world, -20.0F, floorTop, -walkSpeed);
    GameObject& atEnd = makeFallingBody(world, levelWidth - 20.0F, floorTop, walkSpeed);
    world.update(FrameTime{frame, 0});
    passed &= expect(near(atStart.get<Transform>()->x, 0.0F), "a hidden wall closes the start of the level");
    passed &= expect(near(atEnd.get<Transform>()->x, levelWidth - pokemonSize),
                     "a hidden wall closes the end of the level");
    return passed;
}

void run(Boss& boss, float seconds)
{
    for (float t = 0.0F; t < seconds; t += frame) {
        boss.update(frame);
    }
}

bool bossWalksBetweenTheWalls()
{
    Boss boss({}, 1U);
    for (int i = 0; i < 60 * 30; ++i) {
        boss.update(frame);
        if (boss.x() < bossMinX - 0.01F || boss.x() > bossMaxX + 0.01F) {
            return expect(false, "Mewtwo left the walls at x=" + std::to_string(boss.x()));
        }
        if (boss.y() > groundY - bossHeight + 0.01F) {
            return expect(false, "Mewtwo sank through the floor");
        }
    }
    return expect(!boss.fainted(), "nobody hit Mewtwo, so it is still standing");
}

bool bossWalksAcrossAPit()
{
    Boss boss({}, 5U);
    const float pitStart = staticPlatforms[0].x + staticPlatforms[0].width;
    boss.walkFrom(pitStart - bossWidth, false);
    run(boss, 1.5F);
    return expect(boss.x() > staticPlatforms[1].x && near(boss.y(), groundY - bossHeight) &&
                      boss.hp() == bossMaxHp,
                  "Mewtwo walks across a pit without falling in");
}

bool bossJumpsAndLands()
{
    Boss boss({}, 2U);
    const float floorY = groundY - bossHeight;
    boss.jump();
    run(boss, 0.3F);
    bool passed = expect(boss.y() < floorY - 50.0F, "Mewtwo rises after jumping");
    run(boss, 1.2F);
    passed &= expect(near(boss.y(), floorY), "Gravity brings Mewtwo back to the floor");
    return passed;
}

bool shadowBallsFlyOffAndDisappear()
{
    Boss boss({}, 3U);
    boss.fireShadowBall();
    bool passed = expect(boss.shadowBallCount() >= 1, "a shadow ball is a GameObject in Mewtwo's World");
    bool firstOnWire = false;
    for (const Multiplayer::Platform& object : boss.objects()) {
        firstOnWire = firstOnWire || object.id == shadowBallIdBase;
    }
    passed &= expect(firstOnWire, "the shadow ball goes out in the boss world");

    run(boss, 4.0F);
    bool firstGone = true;
    for (const Multiplayer::Platform& object : boss.objects()) {
        firstGone = firstGone && object.id != shadowBallIdBase;
    }
    passed &= expect(firstGone, "a shadow ball that leaves the screen is destroyed");
    return passed;
}

// A HitEvent as the server's bridge raises one: from another machine.
void hit(Boss& boss, std::uint32_t player, const std::string& kind)
{
    Event event{GameEvents::Hit};
    event.origin = player;
    event.set("kind", kind).set("points", 0).set("skill", "test");
    boss.events().raise(std::move(event));
    boss.update(frame);
}

// A stomp at a stomper's pace, which Mewtwo's budget always allows.
void pacedStomp(Boss& boss, std::uint32_t player)
{
    hit(boss, player, GameEvents::HitStomp);
    run(boss, 0.3F);
}

bool damageFaintsAndRespawns()
{
    Boss boss({}, 4U);
    std::vector<Event> deaths;
    boss.events().subscribe(Events::Death, [&](const Event& event) { deaths.push_back(event); });

    hit(boss, 1, GameEvents::HitStomp);
    hit(boss, 1, GameEvents::HitShot);
    bool passed = expect(boss.hp() == bossMaxHp - stompDamage - shotDamage, "each HitEvent takes its damage once");

    while (boss.hp() > stompDamage) {
        pacedStomp(boss, 1);
    }
    hit(boss, 2, GameEvents::HitStomp);
    passed &= expect(boss.fainted() && boss.objects().size() == 1, "a fainted Mewtwo sends only its status");
    passed &= expect(deaths.size() == 1 && deaths[0].origin == 0 && deaths[0].priority == EventPriority::High &&
                         deaths[0].get<std::int64_t>("finisher") == 2 &&
                         deaths[0].get<std::int64_t>("object") == static_cast<std::int64_t>(boss.id()),
                     "the knockout is one High DeathEvent naming the player who finished it");

    hit(boss, 1, GameEvents::HitStomp);
    passed &= expect(deaths.size() == 1, "hits on a fainted Mewtwo raise no second death");

    run(boss, bossRespawnSeconds + 0.1F);
    passed &= expect(!boss.fainted() && boss.hp() == bossMaxHp, "Mewtwo comes back at full health");
    run(boss, 2.0F);
    passed &= expect(near(boss.y(), groundY - bossHeight), "the respawned Mewtwo drops onto the floor");
    return passed;
}

// Mewtwo trusts only hits from the network, and decides their damage itself.
bool bossRefusesBadHits()
{
    Boss boss({}, 6U);
    Event local{GameEvents::Hit};
    local.set("kind", GameEvents::HitStomp);
    boss.events().raise(local);
    boss.update(frame);
    hit(boss, 1, "hyper-beam");
    bool passed = expect(boss.hp() == bossMaxHp && boss.refusedHits() == 1,
                         "a local hit takes no HP, and an unknown kind is refused");

    Event inflated{GameEvents::Hit};
    inflated.origin = 1;
    inflated.set("kind", GameEvents::HitShot).set("damage", 100);
    boss.events().raise(inflated);
    boss.update(frame);
    passed &= expect(boss.hp() == bossMaxHp - shotDamage, "a hit's own damage field is ignored: a shot does 1");

    Event death{Events::Death};
    death.origin = 1;
    death.set("cause", "pit");
    boss.events().raise(death);
    boss.update(frame);
    passed &= expect(!boss.fainted(), "a Pokemon's pit DeathEvent does not knock Mewtwo out");
    return passed;
}

void watchPsystrike(Boss& boss, std::vector<Event>& seen)
{
    boss.events().subscribe(GameEvents::Psystrike, [&seen](const Event& event) { seen.push_back(event); });
}

std::vector<std::string> stagesOf(const std::vector<Event>& seen)
{
    std::vector<std::string> stages;
    for (const Event& event : seen) {
        stages.push_back(event.get<std::string>("stage"));
    }
    return stages;
}

// The charge is announced at once; the strike follows only when the delayed
// PsystrikeDueEvent comes due, and Mewtwo stands still in between.
bool psystrikeChargesThenStrikes()
{
    Boss boss({}, 8U);
    boss.walkFrom(1000.0F, false);
    std::vector<Event> seen;
    watchPsystrike(boss, seen);

    boss.psystrike();
    run(boss, psystrikeChargeSeconds * 0.5F);
    bool passed = expect(stagesOf(seen) == std::vector<std::string>{"charge"} && boss.chargingPsystrike(),
                         "Psystrike announces its charge and has not struck halfway through");
    passed &= expect(near(boss.x(), 1000.0F), "Mewtwo stands still while charging");
    bool flagged = false;
    for (const Multiplayer::Platform& object : boss.objects()) {
        flagged = flagged || (isBoss(object.id) && ((object.id - bossIdBase) & bossPsystrike) != 0);
    }
    passed &= expect(flagged, "the charge is in Mewtwo's id, so every client can draw the aura");
    passed &= expect(!seen.empty() && near(static_cast<float>(seen[0].get<double>("x")), 1000.0F + bossWidth * 0.5F) &&
                         near(static_cast<float>(seen[0].get<double>("seconds")), psystrikeChargeSeconds),
                     "the charge says where Mewtwo is and how long until the strike");

    run(boss, psystrikeChargeSeconds * 0.5F + 0.05F);
    passed &= expect(stagesOf(seen) == std::vector<std::string>{"charge", "strike"} && !boss.chargingPsystrike(),
                     "the strike comes once the charge time has passed");
    passed &= expect(seen.size() == 2 && seen[1].priority == EventPriority::High,
                     "the strike is High priority, so a full queue cannot drop it");
    if (seen.size() == 2) {
        const std::int64_t planned = seen[0].get<std::int64_t>("at");
        const std::int64_t struck = seen[1].get<std::int64_t>("at");
        passed &= expect(struck >= planned && struck - planned < 50'000,
                         "the charge says when the strike will be on Mewtwo's clock, and it strikes then");
    }
    return passed;
}

// The delayed reminder is still queued when Mewtwo faints; it must not strike.
bool faintingCancelsACharge()
{
    Boss boss({}, 9U);
    std::vector<Event> seen;
    watchPsystrike(boss, seen);
    while (boss.hp() > stompDamage) {
        pacedStomp(boss, 1);
    }
    boss.psystrike();
    hit(boss, 2, GameEvents::HitStomp);
    run(boss, psystrikeChargeSeconds + 0.2F);
    return expect(stagesOf(seen) == std::vector<std::string>{"charge"},
                  "a Mewtwo that faints mid-charge never strikes");
}

// Each player gets a budget of damage that refills over time, so a flood of
// hits (more than any Pokemon could land) is refused, and an honest pace is not.
bool bossBudgetsEachPlayer()
{
    Boss boss({}, 11U);
    for (int i = 0; i < 5; ++i) {
        Event event{GameEvents::Hit};
        event.origin = 3;
        event.set("kind", GameEvents::HitStomp);
        boss.events().raise(std::move(event));
    }
    boss.update(frame);
    const int burst = static_cast<int>(Boss::hitBudgetBurst) / stompDamage;
    bool passed = expect(boss.hp() == bossMaxHp - burst * stompDamage && boss.refusedHits() == 5U - burst,
                         "five stomps in one frame: the burst lands, the rest are refused");

    hit(boss, 4, GameEvents::HitStomp);
    passed &= expect(boss.hp() == bossMaxHp - (burst + 1) * stompDamage,
                     "another player's budget is its own");

    run(boss, 1.0F);
    const int before = boss.hp();
    for (int i = 0; i < 3; ++i) {
        pacedStomp(boss, 3);
    }
    passed &= expect(boss.hp() == before - 3 * stompDamage && boss.refusedHits() == 5U - burst,
                     "after a rest, stomps at a stomper's pace all land");
    return passed;
}

// With a locator, a hit must come from where that Pokemon could land it.
bool bossChecksWhereHitsComeFrom()
{
    Boss boss({}, 12U);
    boss.walkFrom(1000.0F, false);
    std::map<std::uint32_t, std::pair<float, float>> poses;
    boss.setPlayerLocator([&poses](std::uint32_t player, float& x, float& y) {
        const auto found = poses.find(player);
        if (found == poses.end()) {
            return false;
        }
        x = found->second.first;
        y = found->second.second;
        return true;
    });
    const auto hpAfter = [&](float x, float y, const char* kind) {
        poses[1] = {x, y};
        const int before = boss.hp();
        hit(boss, 1, kind);
        run(boss, 0.3F);
        return before - boss.hp();
    };
    const float headY = groundY - bossHeight - pokemonSize;
    const float floorY = groundY - pokemonSize;

    bool passed = expect(hpAfter(boss.x() + 20.0F, headY, GameEvents::HitStomp) == stompDamage,
                         "a stomp from on Mewtwo's head lands");
    passed &= expect(hpAfter(boss.x() - 600.0F, floorY, GameEvents::HitStomp) == 0,
                     "a stomp from far along the floor is refused");
    passed &= expect(hpAfter(boss.x() + 20.0F, floorY, GameEvents::HitStomp) == 0,
                     "a stomp from beside Mewtwo's feet is refused");
    passed &= expect(hpAfter(boss.x() - 500.0F, floorY, GameEvents::HitShot) == shotDamage,
                     "a shot from the floor, within a screen, lands");
    passed &= expect(hpAfter(boss.x() - 1200.0F, floorY, GameEvents::HitShot) == 0,
                     "a shot from more than a screen away is refused");
    passed &= expect(hpAfter(boss.x() - 300.0F, 0.0F, GameEvents::HitShot) == 0,
                     "a shot from far above Mewtwo is refused");

    poses.clear();
    const int before = boss.hp();
    hit(boss, 1, GameEvents::HitShot);
    passed &= expect(boss.hp() == before, "a hit from a player the server does not know is refused");
    return passed;
}

// Mewtwo answers a ClockPing with its own clock, addressed to the sender.
bool bossAnswersClockPings()
{
    Boss boss({}, 13U);
    run(boss, 0.5F);
    std::vector<Event> pongs;
    boss.events().subscribe(GameEvents::ClockPong, [&](const Event& event) { pongs.push_back(event); });
    Event ping{GameEvents::ClockPing};
    ping.origin = 5;
    ping.set("sentAt", 42);
    boss.events().raise(std::move(ping));
    boss.update(frame);
    return expect(pongs.size() == 1 && pongs[0].origin == 0 && pongs[0].get<std::int64_t>("player") == 5 &&
                      pongs[0].get<std::int64_t>("sentAt") == 42 &&
                      pongs[0].get<std::int64_t>("bossTime") == boss.clockUs(),
                  "a ClockPing gets one ClockPong with the sender, send time and Mewtwo's clock");
}

bool clockSyncTrustsTheQuickestRoundTrip()
{
    BossClockSync sync;
    bool passed = expect(!sync.synced(), "no estimate before any pong");
    passed &= expect(!sync.receive(1000, 900, 0), "a pong received before its ping was sent is ignored");

    // Mewtwo's clock is ours + 5 s. A 40 ms trip answered at its midpoint...
    const std::int64_t ms = kNsPerMs;
    sync.receive(100 * ms, 140 * ms, (5000 + 120) * 1000);
    passed &= expect(sync.synced() && std::llabs(sync.toLocal((5000 + 140) * 1000) - 140 * ms) < ms,
                     "one round trip places Mewtwo's clock on ours");

    // ...then a slow 400 ms trip answered early, which would be 180 ms off.
    sync.receive(1000 * ms, 1400 * ms, (5000 + 1020) * 1000);
    passed &= expect(sync.roundTrip() == 40 * ms && std::llabs(sync.toLocal(5000 * 1000)) < ms,
                     "the quickest recent round trip is trusted over a slow, lopsided one");
    return passed;
}

// A second charge makes the first one's reminder stale.
bool aNewChargeReplacesTheOld()
{
    Boss boss({}, 10U);
    std::vector<Event> seen;
    watchPsystrike(boss, seen);
    boss.psystrike();
    run(boss, psystrikeChargeSeconds * 0.5F);
    boss.psystrike();
    run(boss, psystrikeChargeSeconds * 0.6F);
    bool passed = expect(stagesOf(seen) == std::vector<std::string>{"charge", "charge"},
                         "the first charge's reminder does not strike for the second");
    run(boss, psystrikeChargeSeconds * 0.5F);
    passed &= expect(stagesOf(seen) == std::vector<std::string>{"charge", "charge", "strike"},
                     "the second charge strikes on its own time");
    return passed;
}

// Between snapshots a client guesses where Mewtwo walks; a charging Mewtwo
// must stay put, or it slides and snaps back when the next snapshot comes.
bool predictionHoldsAChargingMewtwo()
{
    const float floorY = groundY - bossHeight;
    const auto predictedX = [&](std::uint32_t flags) {
        WorldPredictor predictor;
        predictor.receive({{bossIdBase + flags, 1000.0F, floorY, bossWidth, bossHeight}}, 0);
        for (const Multiplayer::Platform& object : predictor.predict(kNsPerSec / 10)) {
            if (isBoss(object.id)) {
                return object.x;
            }
        }
        return -1.0F;
    };
    bool passed = expect(predictedX(0) > 1000.0F + 1.0F, "a walking Mewtwo is predicted to keep walking");
    passed &= expect(near(predictedX(bossCharging), 1000.0F), "a Mewtwo charging a shadow ball stays put");
    passed &= expect(near(predictedX(bossPsystrike), 1000.0F), "a Mewtwo charging Psystrike stays put");
    return passed;
}

bool sharedTypesAreListed()
{
    const std::vector<std::string> types = sharedEventTypes();
    const auto has = [&](const std::string& type) {
        for (const std::string& shared : types) {
            if (shared == type) {
                return true;
            }
        }
        return false;
    };
    return expect(has(GameEvents::Hit) && has(Events::Death) && has(GameEvents::Psystrike) &&
                      has(GameEvents::ClockPing) && has(GameEvents::ClockPong) && !has(GameEvents::PsystrikeDue) &&
                      !has(Events::Input) && !has(Events::Collision),
                  "hits, deaths, Psystrike and clock sync cross the network; input, collisions and Mewtwo's "
                  "reminder do not");
}

} // namespace

int main()
{
    bool passed = true;
    passed &= levelIsGameObjects();
    passed &= pokemonFallsOntoTheFloor();
    passed &= pitsLeadToTheDeathZone();
    passed &= movingPlatformsAreSolid();
    passed &= staticPlatformsAreWallsAndTheLevelHasEnds();
    passed &= bossWalksBetweenTheWalls();
    passed &= bossWalksAcrossAPit();
    passed &= bossJumpsAndLands();
    passed &= shadowBallsFlyOffAndDisappear();
    passed &= damageFaintsAndRespawns();
    passed &= bossRefusesBadHits();
    passed &= psystrikeChargesThenStrikes();
    passed &= faintingCancelsACharge();
    passed &= aNewChargeReplacesTheOld();
    passed &= bossBudgetsEachPlayer();
    passed &= bossChecksWhereHitsComeFrom();
    passed &= bossAnswersClockPings();
    passed &= clockSyncTrustsTheQuickestRoundTrip();
    passed &= predictionHoldsAChargingMewtwo();
    passed &= sharedTypesAreListed();

    if (passed) {
        std::cout << "pokemon-hunt-tests: all checks passed\n";
    }
    return passed ? 0 : 1;
}
